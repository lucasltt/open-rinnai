#pragma once

// Completed uses, independent of peripherals, storage and wall-clock services.
// Snapshot records are oldest-first. Uptime metadata is deliberately not saved:
// without a trusted Unix timestamp a restored use has an unknown age.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include "heater_stats.h"

namespace heater_history {

inline constexpr size_t CAPACITY = 24;
inline constexpr size_t LEGACY_CAPACITY = 12;
inline constexpr uint32_t SNAPSHOT_VERSION = 2;
inline constexpr uint64_t MIN_VALID_EPOCH_S = 1577836800ULL;  // 2020-01-01 UTC.

struct LegacyRecord {
  double water_l{0};
  double energy_kwh{0};
  uint64_t end_epoch_s{0};  // Zero means that wall-clock time was unavailable.
  uint32_t sequence{0};
  uint32_t duration_ms{0};
  uint32_t heating_seconds{0};
  uint8_t complete{0};
  uint8_t end_reason{0};  // heater_stats::EndReason, validated before use.
  uint8_t reserved[2]{0, 0};
};

struct Record {
  double water_l{0};
  double energy_kwh{0};
  uint64_t start_epoch_s{0};  // Legacy records keep zero: their start was not saved.
  uint64_t end_epoch_s{0};
  uint32_t sequence{0};
  uint32_t duration_ms{0};
  uint32_t heating_seconds{0};
  uint8_t complete{0};
  uint8_t end_reason{0};
  uint8_t reserved[2]{0, 0};
};

struct Snapshot {
  uint32_t version{SNAPSHOT_VERSION};
  uint16_t count{0};
  uint16_t reserved{0};
  std::array<Record, CAPACITY> records{};
};

// Firmware <=0.8 stored twelve records. Keep its exact layout independent of
// today's capacity so NVS loads and OTA rollback images remain compatible.
struct LegacySnapshot {
  uint32_t version{1};
  uint16_t count{0};
  uint16_t reserved{0};
  std::array<LegacyRecord, LEGACY_CAPACITY> records{};
};

static_assert(std::is_trivially_copyable<Record>::value, "History record must be NVS-copyable");
static_assert(std::is_standard_layout<Record>::value, "History record must keep a scalar layout");
static_assert(sizeof(Record) == 48, "History record must not acquire implicit padding");
static_assert(offsetof(Record, start_epoch_s) == 16 && offsetof(Record, end_epoch_s) == 24 &&
              offsetof(Record, sequence) == 32 && offsetof(Record, complete) == 44,
              "History record offsets must remain stable");
static_assert(sizeof(LegacyRecord) == 40 && offsetof(LegacyRecord, end_epoch_s) == 16 &&
              offsetof(LegacyRecord, sequence) == 24 && offsetof(LegacyRecord, complete) == 36,
              "Legacy record offsets must remain unchanged");
static_assert(std::is_trivially_copyable<Snapshot>::value, "History snapshot must be NVS-copyable");
static_assert(std::is_standard_layout<Snapshot>::value, "History snapshot must keep a stable layout");
static_assert(sizeof(Snapshot) == 1160 && offsetof(Snapshot, records) == 8,
              "History snapshot must not acquire implicit padding");
static_assert(sizeof(LegacySnapshot) == 488 && offsetof(LegacySnapshot, records) == 8 &&
              std::is_trivially_copyable<LegacySnapshot>::value,
              "Legacy history snapshot layout must remain unchanged");

inline bool valid_epoch(uint64_t epoch_s) { return epoch_s >= MIN_VALID_EPOCH_S; }

class History {
 public:
  size_t count() const { return count_; }

  Record record(size_t oldest_index) const {
    return oldest_index < count_ ? records_[slot_(oldest_index)] : Record{};
  }

  bool add(const heater_stats::Session &session, uint32_t now_ms, uint64_t epoch_s = 0) {
    if (session.active || static_cast<int32_t>(now_ms - session.end_ms) < 0 ||
        static_cast<int32_t>(session.end_ms - session.start_ms) < 0)
      return false;
    Record entry{};
    entry.sequence = session.sequence;
    entry.duration_ms = session.duration_ms;
    entry.heating_seconds = session.heating_seconds;
    entry.water_l = session.water_l;
    entry.energy_kwh = session.energy_kwh;
    entry.complete = session.complete ? 1 : 0;
    entry.end_reason = static_cast<uint8_t>(session.end_reason);
    entry.start_epoch_s = end_epoch_(now_ms, session.start_ms, epoch_s);
    entry.end_epoch_s = end_epoch_(now_ms, session.end_ms, epoch_s);
    if (!valid_record_(entry))
      return false;
    // A repeated or older completion must not duplicate/reorder a persisted use.
    if (count_ != 0 && entry.sequence <= record(count_ - 1).sequence)
      return false;
    records_[next_] = entry;
    transient_[next_] = Transient{session.start_ms, session.end_ms, true};
    next_ = (next_ + 1) % CAPACITY;
    if (count_ < CAPACITY)
      ++count_;
    return true;
  }

  double age_s(size_t oldest_index, uint32_t now_ms, uint64_t epoch_s = 0) const {
    if (oldest_index >= count_)
      return std::numeric_limits<double>::quiet_NaN();
    const size_t slot = slot_(oldest_index);
    const Record &entry = records_[slot];
    if (valid_epoch(epoch_s) && valid_epoch(entry.end_epoch_s) && epoch_s >= entry.end_epoch_s)
      return static_cast<double>(epoch_s - entry.end_epoch_s);
    const Transient &timing = transient_[slot];
    if (timing.current_boot && static_cast<int32_t>(now_ms - timing.end_ms) >= 0)
      return static_cast<uint32_t>(now_ms - timing.end_ms) / 1000.0;
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Only entries from this boot can be dated when the clock first synchronizes.
  // Entries restored without an epoch remain honestly undated.
  bool synchronize_clock(uint32_t now_ms, uint64_t epoch_s) {
    if (!valid_epoch(epoch_s))
      return false;
    bool changed = false;
    for (size_t i = 0; i < count_; ++i) {
      const size_t slot = slot_(i);
      Record &entry = records_[slot];
      if (transient_[slot].current_boot) {
        const uint64_t start = end_epoch_(now_ms, transient_[slot].start_ms, epoch_s);
        const uint64_t end = end_epoch_(now_ms, transient_[slot].end_ms, epoch_s);
        Record dated = entry;
        if (dated.start_epoch_s == 0 && start != 0) dated.start_epoch_s = start;
        if (dated.end_epoch_s == 0 && end != 0) dated.end_epoch_s = end;
        // A later wall-clock correction must not combine a new start with an
        // older end into an impossible interval. Keep the unknown endpoint.
        if (valid_record_(dated) && std::memcmp(&dated, &entry, sizeof(Record)) != 0) {
          entry = dated;
          changed = true;
        }
      }
    }
    return changed;
  }

  Snapshot snapshot() const {
    Snapshot saved{};  // All unused records and explicit reserved bytes are zero.
    saved.count = static_cast<uint16_t>(count_);
    for (size_t i = 0; i < count_; ++i)
      saved.records[i] = record(i);
    return saved;
  }

  LegacySnapshot legacy_snapshot() const {
    LegacySnapshot saved{};
    const auto first = count_ > LEGACY_CAPACITY ? count_ - LEGACY_CAPACITY : 0;
    saved.count = static_cast<uint16_t>(count_ - first);
    for (size_t i = 0; i < saved.count; ++i) {
      const auto entry = record(first + i);
      saved.records[i] = LegacyRecord{entry.water_l, entry.energy_kwh, entry.end_epoch_s,
          entry.sequence, entry.duration_ms, entry.heating_seconds, entry.complete, entry.end_reason, {0, 0}};
    }
    return saved;
  }

  bool restore(const LegacySnapshot &saved) {
    if (saved.version != 1 || saved.reserved != 0 || saved.count > LEGACY_CAPACITY) return false;
    const LegacyRecord blank{};
    for (size_t i = saved.count; i < LEGACY_CAPACITY; ++i)
      if (std::memcmp(&saved.records[i], &blank, sizeof(LegacyRecord)) != 0) return false;
    Snapshot upgraded{};
    upgraded.count = saved.count;
    for (size_t i = 0; i < saved.count; ++i) {
      const auto &entry = saved.records[i];
      if (entry.reserved[0] != 0 || entry.reserved[1] != 0) return false;
      upgraded.records[i] = Record{entry.water_l, entry.energy_kwh, 0, entry.end_epoch_s,
          entry.sequence, entry.duration_ms, entry.heating_seconds, entry.complete, entry.end_reason, {0, 0}};
    }
    return restore(upgraded);
  }

  bool restore(const Snapshot &saved) {
    if (saved.version != SNAPSHOT_VERSION || saved.count > CAPACITY || saved.reserved != 0)
      return false;
    for (size_t i = 0; i < saved.count; ++i) {
      if (!valid_record_(saved.records[i]))
        return false;
      // IDs grow monotonically, including across restarts; duplicates are corrupt.
      if (i > 0 && saved.records[i].sequence <= saved.records[i - 1].sequence)
        return false;
    }
    const Record blank{};
    for (size_t i = saved.count; i < CAPACITY; ++i)
      if (std::memcmp(&saved.records[i], &blank, sizeof(Record)) != 0)
        return false;
    // Validate the full snapshot before replacing any existing history.
    records_ = saved.records;
    transient_ = {};
    count_ = saved.count;
    next_ = count_ % CAPACITY;
    return true;
  }

 private:
  struct Transient {
    uint32_t start_ms{0};
    uint32_t end_ms{0};
    bool current_boot{false};
  };

  size_t slot_(size_t oldest_index) const {
    const size_t oldest = (next_ + CAPACITY - count_) % CAPACITY;
    return (oldest + oldest_index) % CAPACITY;
  }

  static uint64_t end_epoch_(uint32_t now_ms, uint32_t end_ms, uint64_t epoch_s) {
    if (!valid_epoch(epoch_s) || static_cast<int32_t>(now_ms - end_ms) < 0)
      return 0;
    const uint64_t age = static_cast<uint32_t>(now_ms - end_ms) / 1000U;
    return age <= epoch_s - MIN_VALID_EPOCH_S ? epoch_s - age : 0;
  }

  static bool valid_record_(const Record &entry) {
    const auto flow_stop = static_cast<uint8_t>(heater_stats::EndReason::FLOW_STOP);
    const auto gap = static_cast<uint8_t>(heater_stats::EndReason::INTEGRATION_GAP);
    return entry.sequence != 0 && std::isfinite(entry.water_l) && entry.water_l >= 0 &&
           std::isfinite(entry.energy_kwh) && entry.energy_kwh >= 0 &&
           (entry.start_epoch_s == 0 || valid_epoch(entry.start_epoch_s)) &&
           (entry.end_epoch_s == 0 || valid_epoch(entry.end_epoch_s)) &&
           (entry.start_epoch_s == 0 || entry.end_epoch_s == 0 || entry.start_epoch_s <= entry.end_epoch_s) &&
           entry.heating_seconds <= entry.duration_ms / 1000U &&
           entry.complete <= 1 && entry.end_reason >= flow_stop && entry.end_reason <= gap &&
           (entry.complete != 0) == (entry.end_reason == flow_stop) &&
           entry.reserved[0] == 0 && entry.reserved[1] == 0;
  }

  std::array<Record, CAPACITY> records_{};
  std::array<Transient, CAPACITY> transient_{};
  size_t count_{0};
  size_t next_{0};
};

}  // namespace heater_history
