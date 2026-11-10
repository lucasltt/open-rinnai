#pragma once
#include <cstring>
#include <ctime>
#include "esphome/core/preferences.h"
#include "esphome/core/helpers.h"
#include "heater_stats.h"
#include "heater_history.h"
#include "heater_periods.h"
#include "heater_health.h"
#include "heater_sessions.h"

namespace rinnai_storage {
struct SettingsPersist {
  uint32_t version{1}, gas_type{0};
  double gas_tariff{0}, water_tariff{0};
};
static_assert(sizeof(SettingsPersist) == 24, "Stable settings storage layout");
struct Archive {
  uint32_t version{1}, reserved{0};
  heater_stats::Persist totals{};
  heater_history::LegacySnapshot history{};
};
static_assert(sizeof(Archive) == 528, "Stable accounting and history archive layout");
static_assert(std::is_trivially_copyable<Archive>::value, "Archive must be NVS-copyable");
struct ExtendedArchiveV2 {
  uint32_t version{2}, reserved{0};
  heater_stats::Persist totals{};
  heater_history::LegacySnapshot history{};
  heater_periods::Snapshot periods{};
};
static_assert(sizeof(ExtendedArchiveV2) == 1952, "Legacy periods archive layout must remain unchanged");
struct ExtendedArchive {
  uint32_t version{3}, reserved{0};
  heater_stats::Persist totals{};
  heater_history::Snapshot history{};
  heater_periods::Snapshot periods{};
};
static_assert(sizeof(ExtendedArchive) == 2624, "Stable 24-use periods archive layout");
static_assert(std::is_trivially_copyable<ExtendedArchive>::value, "Archive must be NVS-copyable");
// The stream identity and the complete accounting checkpoint are one durable
// NVS value. V3 is still written separately for compatible OTA rollback.
struct StreamArchive {
  uint32_t version{4}, reserved{0};
  std::array<uint8_t, 16> stream_id{};
  ExtendedArchive data{};
};
static_assert(sizeof(StreamArchive) == 2648 && std::is_trivially_copyable<StreamArchive>::value,
              "Stable sessions stream archive layout");
inline heater_history::History history{};
inline heater_periods::Periods periods{};
inline heater_health::Monitor health{};
inline esphome::ESPPreferenceObject totals_preference{}, settings_preference{}, archive_preference{};
inline esphome::ESPPreferenceObject extended_preference{}, health_preference{}, preferences_preference{};
inline esphome::ESPPreferenceObject legacy_extended_preference{};
inline esphome::ESPPreferenceObject stream_preference{};
inline Archive last_saved{};
// Fixed work buffers keep large NVS snapshots off the ESP main task's stack.
inline ExtendedArchive extended_work{}, extended_last_saved{};
inline StreamArchive stream_work{};
inline std::array<uint8_t, 16> stream_id{};
inline ExtendedArchiveV2 legacy_extended_work{};
inline heater_history::History rollback_incoming{};
inline heater_history::Snapshot rollback_merged{};
inline heater_health::Snapshot health_last_saved{};
inline uint32_t last_saved_ms{0}, last_attempt_ms{0};
inline bool initialized{false}, storage_ok{true}, archive_loaded{false}, archive_saved{false};
inline bool health_storage_ok{true}, preferences_storage_ok{true};
inline bool stream_ready{false}, session_storage_ok{true};

inline bool valid_stream_id(const std::array<uint8_t, 16> &value) {
  for (const auto byte : value) if (byte != 0) return true;
  return false;
}

inline bool generate_stream_id() {
  stream_id = {};
  for (size_t i = 0; i < stream_id.size(); i += sizeof(uint32_t)) {
    const uint32_t random = esphome::random_uint32();
    std::memcpy(stream_id.data() + i, &random, sizeof(random));
  }
  return valid_stream_id(stream_id);
}

inline uint64_t epoch_now() {
  const auto now = std::time(nullptr);
  return now >= static_cast<time_t>(heater_history::MIN_VALID_EPOCH_S) ? static_cast<uint64_t>(now) : 0;
}

inline Archive snapshot() {
  Archive result{};
  result.totals = heater_stats::persist_snapshot();
  result.history = history.legacy_snapshot();
  return result;
}

inline void fill_extended(ExtendedArchive &out) {
  out.version = 3;
  out.reserved = 0;
  out.totals = heater_stats::persist_snapshot();
  out.history = history.snapshot();
  out.periods = periods.snapshot();
}

inline void fill_legacy_extended(ExtendedArchiveV2 &out) {
  out.version = 2;
  out.reserved = 0;
  out.totals = heater_stats::persist_snapshot();
  out.history = history.legacy_snapshot();
  out.periods = periods.snapshot();
}

inline bool save_health(bool force = false) {
  if (!initialized) return false;
  const auto current = health.snapshot();
  if (!force && health_storage_ok && std::memcmp(&current, &health_last_saved, sizeof(current)) == 0) return true;
  health_storage_ok = health_preference.save(&current) && esphome::global_preferences->sync();
  if (health_storage_ok) health_last_saved = current;
  return health_storage_ok;
}

inline bool adopt_newer_rollback_checkpoint(const ExtendedArchive &current) {
  if (!legacy_extended_preference.load(&legacy_extended_work) || legacy_extended_work.version != 2 ||
      legacy_extended_work.reserved != 0) return false;
  const auto &prior = current.totals;
  const auto &latest = legacy_extended_work.totals;
  const bool valid_totals = latest.version == 1 && std::isfinite(latest.water_l) && latest.water_l >= 0 &&
      std::isfinite(latest.energy_kwh) && latest.energy_kwh >= 0 && latest.heating_remainder_ms < 1000;
  const bool monotonic = latest.water_l >= prior.water_l && latest.energy_kwh >= prior.energy_kwh &&
      latest.heating_seconds >= prior.heating_seconds && latest.sessions >= prior.sessions;
  const bool advanced = latest.water_l > prior.water_l || latest.energy_kwh > prior.energy_kwh ||
      latest.heating_seconds > prior.heating_seconds || latest.sessions > prior.sessions;
  if (!valid_totals || !monotonic || !advanced) return false;
  auto &incoming = rollback_incoming;
  if (!incoming.restore(legacy_extended_work.history) ||
      (incoming.count() && incoming.record(incoming.count() - 1).sequence > latest.sessions) ||
      !periods.restore(legacy_extended_work.periods)) return false;
  auto &merged = rollback_merged;
  merged = history.snapshot();
  for (size_t i = 0; i < incoming.count(); ++i) {
    const auto record = incoming.record(i);
    if (merged.count && record.sequence <= merged.records[merged.count - 1].sequence) continue;
    if (merged.count == heater_history::CAPACITY) {
      for (size_t j = 1; j < merged.count; ++j) merged.records[j - 1] = merged.records[j];
      --merged.count;
    }
    merged.records[merged.count++] = record;  // Older-format new uses have unknown starts.
  }
  if (!history.restore(merged)) {
    periods.restore(current.periods);
    return false;
  }
  heater_stats::restore(latest);
  return true;
}

// A 1.1 rollback can advance its V3 checkpoint while this V4 stream remains
// on flash. Preserve the stream for a genuine monotonic extension, retaining
// the known starts of records already present in V4.
inline bool adopt_newer_v3_checkpoint(const ExtendedArchive &current) {
  if (!extended_preference.load(&extended_work) || extended_work.version != 3 ||
      extended_work.reserved != 0) return false;
  const auto &latest = extended_work.totals;
  const auto &prior = current.totals;
  const bool valid_totals = latest.version == 1 && std::isfinite(latest.water_l) && latest.water_l >= 0 &&
      std::isfinite(latest.energy_kwh) && latest.energy_kwh >= 0 && latest.heating_remainder_ms < 1000;
  const bool monotonic = latest.water_l >= prior.water_l && latest.energy_kwh >= prior.energy_kwh &&
      latest.heating_seconds >= prior.heating_seconds && latest.sessions >= prior.sessions;
  const bool advanced = latest.water_l > prior.water_l || latest.energy_kwh > prior.energy_kwh ||
      latest.heating_seconds > prior.heating_seconds || latest.sessions > prior.sessions;
  if (!valid_totals || !monotonic || !advanced || !rollback_incoming.restore(extended_work.history) ||
      (rollback_incoming.count() && rollback_incoming.record(rollback_incoming.count() - 1).sequence > latest.sessions) ||
      !periods.restore(extended_work.periods)) return false;
  auto &merged = rollback_merged;
  merged = history.snapshot();
  bool identity_conflict = false;
  for (size_t i = 0; i < rollback_incoming.count(); ++i) {
    const auto row = rollback_incoming.record(i);
    for (size_t j = 0; j < merged.count; ++j) {
      const auto &old = merged.records[j];
      if (old.sequence == row.sequence && (old.water_l != row.water_l || old.energy_kwh != row.energy_kwh ||
          old.duration_ms != row.duration_ms || old.heating_seconds != row.heating_seconds ||
          old.complete != row.complete || old.end_reason != row.end_reason)) identity_conflict = true;
    }
    if (merged.count && row.sequence <= merged.records[merged.count - 1].sequence) continue;
    if (merged.count == heater_history::CAPACITY) {
      for (size_t j = 1; j < merged.count; ++j) merged.records[j - 1] = merged.records[j];
      --merged.count;
    }
    merged.records[merged.count++] = row;
  }
  if (identity_conflict) {
    // A reset/fork is not a continuation of the old IDs. Keep the candidate
    // records as a new stream instead of silently changing an existing use.
    merged = extended_work.history;
    stream_ready = false;
    if (!generate_stream_id()) session_storage_ok = false;
  }
  if (!history.restore(merged)) {
    periods.restore(current.periods);
    return false;
  }
  heater_stats::restore(latest);
  return true;
}

inline bool restore_extended(const ExtendedArchive &saved) {
  return saved.version == 3 && saved.reserved == 0 && history.restore(saved.history) &&
      periods.restore(saved.periods) &&
      (history.count() == 0 || history.record(history.count() - 1).sequence <= saved.totals.sessions) &&
      heater_stats::restore(saved.totals);
}

inline void begin(uint32_t reset_reason = 0) {
  if (initialized || !esphome::global_preferences) return;
  history = heater_history::History{};
  periods = heater_periods::Periods{};
  health = heater_health::Monitor{};
  archive_loaded = archive_saved = false;
  storage_ok = health_storage_ok = preferences_storage_ok = true;
  stream_ready = false;
  session_storage_ok = true;
  stream_id = {};
  totals_preference = esphome::global_preferences->make_preference<heater_stats::Persist>(0x6EA91C52);
  settings_preference = esphome::global_preferences->make_preference<SettingsPersist>(0x6EA91C53);
  archive_preference = esphome::global_preferences->make_preference<Archive>(0x6EA91C54);
  extended_preference = esphome::global_preferences->make_preference<ExtendedArchive>(0x6EA91C5A);
  legacy_extended_preference = esphome::global_preferences->make_preference<ExtendedArchiveV2>(0x6EA91C55);
  preferences_preference = esphome::global_preferences->make_preference<heater_health::Settings>(0x6EA91C56);
  health_preference = esphome::global_preferences->make_preference<heater_health::Snapshot>(0x6EA91C57);
  stream_preference = esphome::global_preferences->make_preference<StreamArchive>(0x6EA91C5B);
  const bool loaded_stream = stream_preference.load(&stream_work) && stream_work.version == 4 &&
      stream_work.reserved == 0 && valid_stream_id(stream_work.stream_id) && restore_extended(stream_work.data);
  if (loaded_stream) {
    stream_id = stream_work.stream_id;
    extended_last_saved = stream_work.data;
    stream_ready = true;
    archive_loaded = archive_saved = true;
    if (adopt_newer_v3_checkpoint(extended_last_saved)) archive_saved = false;
    fill_extended(extended_work);
    if (adopt_newer_rollback_checkpoint(extended_work)) archive_saved = false;
  } else {
    // Without a valid V4 value no old generation can safely be reused, even
    // when a compatible older checkpoint still contains the same sequence.
    history = heater_history::History{};
    periods = heater_periods::Periods{};
    session_storage_ok = generate_stream_id();
    const bool loaded_extended = extended_preference.load(&extended_work) && restore_extended(extended_work);
    if (loaded_extended) {
      archive_loaded = true;
      // A rollback image can have recorded additional uses since this v3 file.
      // Keep known starts and append records from the compatible checkpoint.
      adopt_newer_rollback_checkpoint(extended_work);
    } else {
      history = heater_history::History{};
      periods = heater_periods::Periods{};
      const bool loaded_v2 = legacy_extended_preference.load(&legacy_extended_work) &&
          legacy_extended_work.version == 2 && legacy_extended_work.reserved == 0 &&
          history.restore(legacy_extended_work.history) && periods.restore(legacy_extended_work.periods) &&
          (history.count() == 0 || history.record(history.count() - 1).sequence <= legacy_extended_work.totals.sessions) &&
          heater_stats::restore(legacy_extended_work.totals);
      if (loaded_v2) {
        archive_loaded = true;
      } else {
        history = heater_history::History{};
        periods = heater_periods::Periods{};
        Archive stored{};
        heater_history::History candidate;
        if (archive_preference.load(&stored) && stored.version == 1 && stored.reserved == 0 &&
            candidate.restore(stored.history) &&
            (candidate.count() == 0 || candidate.record(candidate.count() - 1).sequence <= stored.totals.sessions) &&
            heater_stats::restore(stored.totals)) {
          history = candidate;
          archive_loaded = true;
        } else {
          // Migrate old totals without inventing sessions lost in old RAM.
          heater_stats::Persist legacy{};
          if (totals_preference.load(&legacy)) heater_stats::restore(legacy);
        }
      }
    }
  }
  SettingsPersist settings{};
  if (settings_preference.load(&settings) && settings.version == 1) {
    heater_stats::Settings config;
    config.gas_type = settings.gas_type == 1 ? heater_stats::GasType::GLP : heater_stats::GasType::GN;
    config.gas_tariff_per_unit = settings.gas_tariff;
    config.water_tariff_per_m3 = settings.water_tariff;
    heater_stats::configure(config);
  }
  heater_health::Settings preferences{};
  if (preferences_preference.load(&preferences)) health.configure(preferences);
  heater_health::Snapshot boots{};
  if (health_preference.load(&boots)) health.restore(boots);
  periods.seed(heater_stats::totals(), epoch_now());
  health.begin_boot(reset_reason, epoch_now(), esphome::millis());
  last_saved = snapshot();
  // Only a verified loaded V4 can be used as the durable export checkpoint.
  // A migration or rollback extension becomes visible after save_totals syncs.
  if (!loaded_stream) extended_last_saved = ExtendedArchive{};
  last_saved_ms = esphome::millis();
  initialized = true;
  save_health(true);
}

inline bool save_totals(bool force = false) {
  if (!initialized) return false;
  const auto now = esphome::millis();
  if (!force && archive_saved && storage_ok && session_storage_ok &&
      static_cast<uint32_t>(now - last_saved_ms) < 600000U) return true;
  if (!force && (!storage_ok || !session_storage_ok) && static_cast<uint32_t>(now - last_attempt_ms) < 10000U) return false;
  fill_extended(extended_work);
  if (archive_saved && storage_ok && session_storage_ok &&
      std::memcmp(&extended_work, &extended_last_saved, sizeof(extended_work)) == 0) {
    last_saved_ms = now;
    return true;
  }
  last_attempt_ms = now;
  // One archive holds both history and counters. Keep legacy counters current
  // as a fallback for older firmware, without relying on two-key consistency.
  const auto current = snapshot();
  fill_legacy_extended(legacy_extended_work);
  stream_work.version = 4;
  stream_work.reserved = 0;
  stream_work.stream_id = stream_id;
  stream_work.data = extended_work;
  session_storage_ok = valid_stream_id(stream_id) && stream_preference.save(&stream_work) &&
      extended_preference.save(&extended_work) && legacy_extended_preference.save(&legacy_extended_work) &&
      archive_preference.save(&current) && totals_preference.save(&current.totals) &&
      esphome::global_preferences->sync();
  storage_ok = session_storage_ok;
  if (session_storage_ok) {
    last_saved = current;
    extended_last_saved = extended_work;
    last_saved_ms = now;
    archive_saved = true;
    stream_ready = true;
  }
  return storage_ok;
}

// Called only by the main loop, before transferring the POD to the HTTP
// backend. Uncommitted RAM records and mutable timestamps are never exported.
inline void copy_sessions_snapshot(rinnai_sessions::Snapshot &out) {
  out.ready = initialized && stream_ready && archive_saved;
  out.storage_ok = storage_ok && session_storage_ok;
  if (!out.ready || !out.storage_ok) return;
  out.history = extended_last_saved.history;
  out.stream_id = stream_id;
  out.total_sessions = extended_last_saved.totals.sessions;
  out.settings = heater_stats::settings();
}

inline bool configure(const heater_stats::Settings &settings) {
  const auto previous = heater_stats::settings();
  heater_stats::configure(settings);
  if (!initialized) return false;
  const auto actual = heater_stats::settings();
  SettingsPersist saved;
  saved.gas_type = actual.gas_type == heater_stats::GasType::GLP ? 1 : 0;
  saved.gas_tariff = actual.gas_tariff_per_unit;
  saved.water_tariff = actual.water_tariff_per_m3;
  storage_ok = settings_preference.save(&saved) && esphome::global_preferences->sync();
  if (!storage_ok) {
    heater_stats::configure(previous);
    SettingsPersist rollback{};
    rollback.gas_type = previous.gas_type == heater_stats::GasType::GLP ? 1 : 0;
    rollback.gas_tariff = previous.gas_tariff_per_unit;
    rollback.water_tariff = previous.water_tariff_per_m3;
    settings_preference.save(&rollback);
  }
  return storage_ok;
}

inline bool configure_preferences(const heater_health::Settings &settings) {
  if (!initialized || !heater_health::valid_settings(settings)) return false;
  const auto previous = health.settings();
  preferences_storage_ok = preferences_preference.save(&settings) && esphome::global_preferences->sync();
  if (preferences_storage_ok) health.configure(settings);
  else preferences_preference.save(&previous);
  return preferences_storage_ok;
}
}  // namespace rinnai_storage
