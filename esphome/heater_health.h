#pragma once

// Portable boot history and local alert evaluation. No I/O, NVS, networking,
// heater commands or wall-clock reads; callers supply both clocks explicitly.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "heater_stats.h"

namespace heater_health {

inline constexpr size_t BOOT_CAPACITY = 12;
inline constexpr uint64_t MIN_VALID_EPOCH_S = 1577836800ULL;
inline constexpr uint32_t LONG_USE = 1U;
inline constexpr uint32_t CONTINUOUS_FLOW = 2U;
inline constexpr uint32_t HEATER_ERROR = 4U;
inline constexpr uint32_t OFFLINE = 8U;
inline constexpr uint32_t ALL_ALERTS = LONG_USE | CONTINUOUS_FLOW | HEATER_ERROR | OFFLINE;

struct Favorite {
  char name[32]{};
  int32_t temperature_c{38};
};

struct Settings {
  uint32_t version{1};
  uint32_t enabled_mask{ALL_ALERTS};
  uint32_t long_use_min{15};
  uint32_t continuous_flow_min{20};
  uint32_t offline_seconds{30};
  uint32_t reserved{0};
  std::array<Favorite, 3> favorites{{{"Banho", 38}, {"Conforto", 39}, {"Cozinha", 40}}};
};

struct BootRecord {
  uint64_t epoch_s{0};  // Zero: boot time could not be established.
  uint32_t sequence{0};
  uint32_t reason_code{0};  // esp_reset_reason_t supplied by caller, not read here.
};

struct Snapshot {
  uint32_t version{1};
  uint32_t boot_count{0};
  uint32_t crash_count{0};
  uint16_t count{0};
  uint16_t reserved{0};
  std::array<BootRecord, BOOT_CAPACITY> boots{};  // Oldest first.
};

static_assert(std::is_trivially_copyable<Settings>::value && std::is_standard_layout<Settings>::value,
              "Settings must remain NVS-copyable");
static_assert(sizeof(Favorite) == 36 && sizeof(Settings) == 132 && offsetof(Settings, favorites) == 24,
              "Settings must keep their explicit scalar layout");
static_assert(std::is_trivially_copyable<Snapshot>::value && std::is_standard_layout<Snapshot>::value,
              "Boot snapshot must remain NVS-copyable");
static_assert(sizeof(BootRecord) == 16 && sizeof(Snapshot) == 208 && offsetof(Snapshot, boots) == 16,
              "Boot snapshot must keep its scalar layout");

inline bool valid_epoch(uint64_t epoch_s) { return epoch_s >= MIN_VALID_EPOCH_S; }

inline bool is_crash_reason(uint32_t reason) {
  // ESP-IDF panic, interrupt/task/general watchdog, CPU lockup. Brownout and
  // power-glitch remain separate reset reasons, rather than claimed crashes.
  return (reason >= 4 && reason <= 7) || reason == 15;
}

inline bool valid_name(const char (&name)[32]) {
  size_t length = 0;
  bool has_visible = false;
  while (length < sizeof(name) && name[length] != '\0') {
    const auto value = static_cast<uint8_t>(name[length]);
    if (value < 0x20 || value == 0x7F) return false;
    if (value != ' ') has_visible = true;
    ++length;
  }
  if (length == 0 || length >= sizeof(name) || !has_visible) return false;
  // Validate UTF-8 so a corrupt persistent name cannot break JSON or display.
  for (size_t i = 0; i < length;) {
    const auto lead = static_cast<uint8_t>(name[i]);
    if (lead < 0x80) { ++i; continue; }
    unsigned follow = 0;
    uint8_t minimum = 0x80, maximum = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) follow = 1;
    else if (lead >= 0xE0 && lead <= 0xEF) {
      follow = 2;
      if (lead == 0xE0) minimum = 0xA0;
      if (lead == 0xED) maximum = 0x9F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      follow = 3;
      if (lead == 0xF0) minimum = 0x90;
      if (lead == 0xF4) maximum = 0x8F;
    } else return false;
    if (i + follow >= length) return false;
    const auto second = static_cast<uint8_t>(name[i + 1]);
    if (second < minimum || second > maximum) return false;
    for (unsigned j = 2; j <= follow; ++j) {
      const auto continuation = static_cast<uint8_t>(name[i + j]);
      if (continuation < 0x80 || continuation > 0xBF) return false;
    }
    i += follow + 1;
  }
  return true;
}

inline bool valid_settings(const Settings &settings) {
  if (settings.version != 1 || settings.reserved != 0 || (settings.enabled_mask & ~ALL_ALERTS) != 0 ||
      settings.long_use_min < 1 || settings.long_use_min > 180 || settings.continuous_flow_min < 1 ||
      settings.continuous_flow_min > 180 || settings.offline_seconds < 10 || settings.offline_seconds > 600)
    return false;
  for (const auto &favorite : settings.favorites)
    if (!valid_name(favorite.name) || favorite.temperature_c < 35 || favorite.temperature_c > 50) return false;
  return true;
}

struct Metrics {
  bool online{false};
  bool ever_seen{false};
  bool error_known{false};
  bool flow_fresh{false};
  uint32_t last_valid_ms{0};
  uint16_t error_code{0};
  heater_stats::Session session{};
};

struct Alerts {
  bool long_use{false};
  bool continuous_flow{false};
  bool heater_error{false};
  bool offline{false};
  bool heater_error_stale{false};
  uint8_t active_count{0};
};

class Monitor {
 public:
  bool begin_boot(uint32_t reason_code, uint64_t epoch_s, uint32_t now_ms) {
    if (reason_code > 255 || boot_count_ == UINT32_MAX) return false;
    ++boot_count_;
    if (is_crash_reason(reason_code)) ++crash_count_;
    BootRecord record{};
    record.sequence = boot_count_;
    record.reason_code = reason_code;
    record.epoch_s = boot_epoch_(now_ms, epoch_s);
    boots_[next_] = record;
    current_slot_ = next_;
    next_ = (next_ + 1) % BOOT_CAPACITY;
    if (count_ < BOOT_CAPACITY) ++count_;
    current_boot_ = true;
    boot_start_ms_ = now_ms;
    grace_started_ = true;
    error_active_ = false;
    alerts_ = Alerts{};
    return true;
  }

  // The supplied now_ms is uptime, so clock sync can date power-on without
  // assigning the synchronization moment itself as the boot time.
  bool synchronize_clock(uint32_t now_ms, uint64_t epoch_s) {
    if (!current_boot_ || boots_[current_slot_].epoch_s != 0) return false;
    const auto boot_epoch = boot_epoch_(now_ms, epoch_s);
    if (boot_epoch == 0) return false;
    boots_[current_slot_].epoch_s = boot_epoch;
    return true;
  }

  uint32_t boot_count() const { return boot_count_; }
  uint32_t crash_count() const { return crash_count_; }
  size_t boot_size() const { return count_; }
  BootRecord boot(size_t oldest_index) const {
    return oldest_index < count_ ? boots_[slot_(oldest_index)] : BootRecord{};
  }

  Settings settings() const { return settings_; }
  bool configure(const Settings &settings) {
    if (!valid_settings(settings)) return false;
    settings_ = settings;
    return true;
  }

  Alerts update(uint32_t now_ms, const Metrics &metrics) {
    if (!grace_started_) { boot_start_ms_ = now_ms; grace_started_ = true; }
    if (metrics.error_known) error_active_ = metrics.error_code != 0;
    const bool flowing = metrics.online && metrics.flow_fresh && metrics.session.active;
    alerts_.long_use = (settings_.enabled_mask & LONG_USE) && flowing && metrics.session.heating_seconds > 0 &&
        metrics.session.duration_ms >= settings_.long_use_min * 60000U;
    alerts_.continuous_flow = (settings_.enabled_mask & CONTINUOUS_FLOW) && flowing &&
        metrics.session.duration_ms >= settings_.continuous_flow_min * 60000U;
    alerts_.heater_error = (settings_.enabled_mask & HEATER_ERROR) && error_active_;
    // Missing readings cannot establish that a previously observed error cleared.
    alerts_.heater_error_stale = alerts_.heater_error && !metrics.error_known;
    const auto offline_since = metrics.ever_seen ? metrics.last_valid_ms : boot_start_ms_;
    alerts_.offline = (settings_.enabled_mask & OFFLINE) && !metrics.online &&
        static_cast<uint32_t>(now_ms - offline_since) >= settings_.offline_seconds * 1000U;
    alerts_.active_count = static_cast<uint8_t>(alerts_.long_use + alerts_.continuous_flow +
        alerts_.heater_error + alerts_.offline);
    return alerts_;
  }
  Alerts alerts() const { return alerts_; }

  Snapshot snapshot() const {
    Snapshot saved{};
    saved.boot_count = boot_count_;
    saved.crash_count = crash_count_;
    saved.count = static_cast<uint16_t>(count_);
    for (size_t i = 0; i < count_; ++i) saved.boots[i] = boot(i);
    return saved;
  }

  bool restore(const Snapshot &saved) {
    const size_t expected_count = saved.boot_count < BOOT_CAPACITY ? saved.boot_count : BOOT_CAPACITY;
    if (saved.version != 1 || saved.reserved != 0 || saved.count != expected_count ||
        saved.crash_count > saved.boot_count) return false;
    uint32_t visible_crashes = 0;
    for (size_t i = 0; i < saved.count; ++i) {
      const auto &record = saved.boots[i];
      if (record.reason_code > 255 || (record.epoch_s != 0 && !valid_epoch(record.epoch_s)) ||
          record.sequence != saved.boot_count - saved.count + i + 1) return false;
      if (is_crash_reason(record.reason_code)) ++visible_crashes;
    }
    if (visible_crashes > saved.crash_count) return false;
    const BootRecord blank{};
    for (size_t i = saved.count; i < BOOT_CAPACITY; ++i)
      if (std::memcmp(&saved.boots[i], &blank, sizeof(blank)) != 0) return false;
    boots_ = saved.boots;
    boot_count_ = saved.boot_count;
    crash_count_ = saved.crash_count;
    count_ = saved.count;
    next_ = count_ % BOOT_CAPACITY;
    current_boot_ = grace_started_ = error_active_ = false;
    alerts_ = Alerts{};
    return true;
  }

 private:
  size_t slot_(size_t oldest_index) const { return (next_ + BOOT_CAPACITY - count_ + oldest_index) % BOOT_CAPACITY; }
  static uint64_t boot_epoch_(uint32_t now_ms, uint64_t epoch_s) {
    if (!valid_epoch(epoch_s)) return 0;
    const uint64_t elapsed_s = now_ms / 1000U;
    return elapsed_s <= epoch_s - MIN_VALID_EPOCH_S ? epoch_s - elapsed_s : 0;
  }
  Settings settings_{};
  Alerts alerts_{};
  std::array<BootRecord, BOOT_CAPACITY> boots_{};
  uint32_t boot_count_{0}, crash_count_{0}, boot_start_ms_{0};
  size_t count_{0}, next_{0}, current_slot_{0};
  bool current_boot_{false}, grace_started_{false}, error_active_{false};
};

}  // namespace heater_health
