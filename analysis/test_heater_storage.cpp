#include "../esphome/heater_storage.h"

#include <cstdlib>
#include <iostream>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; \
  std::exit(EXIT_FAILURE); } } while (false)

static constexpr uint32_t TOTALS_KEY = 0x6EA91C52;
static constexpr uint32_t SETTINGS_KEY = 0x6EA91C53;
static constexpr uint32_t OLD_ARCHIVE_KEY = 0x6EA91C54;
static constexpr uint32_t V2_ARCHIVE_KEY = 0x6EA91C55;
static constexpr uint32_t ARCHIVE_KEY = 0x6EA91C5A;
static constexpr uint32_t STREAM_KEY = 0x6EA91C5B;
static constexpr uint32_t PREFERENCES_KEY = 0x6EA91C56;
static constexpr uint32_t BOOT_KEY = 0x6EA91C57;
static constexpr uint64_t EPOCH = 1793000000ULL;

static void reboot(esphome::ESPPreferences &preferences, uint32_t millis = 1000) {
  // NVS survives. RAM state, preference handles and pending writes do not.
  preferences.pending.clear();
  preferences.allow_save = preferences.allow_sync = true;
  esphome::global_preferences = &preferences;
  esphome::test_millis = millis;
  heater_stats::detail::accounting = heater_stats::detail::Accounting{};
  rinnai_storage::history = heater_history::History{};
  rinnai_storage::periods = heater_periods::Periods{};
  rinnai_storage::health = heater_health::Monitor{};
  rinnai_storage::totals_preference = esphome::ESPPreferenceObject{};
  rinnai_storage::settings_preference = esphome::ESPPreferenceObject{};
  rinnai_storage::archive_preference = esphome::ESPPreferenceObject{};
  rinnai_storage::extended_preference = esphome::ESPPreferenceObject{};
  rinnai_storage::legacy_extended_preference = esphome::ESPPreferenceObject{};
  rinnai_storage::preferences_preference = esphome::ESPPreferenceObject{};
  rinnai_storage::health_preference = esphome::ESPPreferenceObject{};
  rinnai_storage::stream_preference = esphome::ESPPreferenceObject{};
  rinnai_storage::stream_work = rinnai_storage::StreamArchive{};
  rinnai_storage::stream_id = {};
  rinnai_storage::stream_ready = false;
  rinnai_storage::session_storage_ok = true;
  rinnai_storage::last_saved = rinnai_storage::Archive{};
  rinnai_storage::extended_work = rinnai_storage::ExtendedArchive{};
  rinnai_storage::legacy_extended_work = rinnai_storage::ExtendedArchiveV2{};
  rinnai_storage::extended_last_saved = rinnai_storage::ExtendedArchive{};
  rinnai_storage::health_last_saved = heater_health::Snapshot{};
  rinnai_storage::last_saved_ms = rinnai_storage::last_attempt_ms = 0;
  rinnai_storage::initialized = false;
  rinnai_storage::storage_ok = true;
  rinnai_storage::health_storage_ok = rinnai_storage::preferences_storage_ok = true;
  rinnai_storage::archive_loaded = rinnai_storage::archive_saved = false;
  rinnai_storage::begin();
}

static heater_stats::Persist legacy() {
  heater_stats::Persist totals;
  totals.water_l = 214.25;
  totals.energy_kwh = 5.625;
  totals.heating_seconds = 3600;
  totals.sessions = 5;
  totals.heating_remainder_ms = 123;
  return totals;
}

static rinnai_storage::SettingsPersist settings() {
  rinnai_storage::SettingsPersist saved;
  saved.gas_type = 0;
  saved.gas_tariff = 8.125;
  saved.water_tariff = 10.75;
  return saved;
}

static heater_stats::Session session(uint32_t sequence) {
  heater_stats::Session result;
  result.sequence = sequence;
  result.start_ms = sequence * 70000U;
  result.end_ms = result.start_ms + 60000U;
  result.duration_ms = 60000;
  result.heating_seconds = 55;
  result.water_l = 10;
  result.energy_kwh = 0.125;
  result.complete = true;
  result.end_reason = heater_stats::EndReason::FLOW_STOP;
  return result;
}

static void legacy_migration_and_settings_survive() {
  esphome::ESPPreferences preferences;
  const auto original_totals = legacy();
  const auto original_settings = settings();
  preferences.seed(TOTALS_KEY, original_totals);
  preferences.seed(SETTINGS_KEY, original_settings);
  reboot(preferences);
  CHECK(rinnai_storage::initialized);
  CHECK(!rinnai_storage::archive_loaded);
  CHECK(!rinnai_storage::archive_saved);
  CHECK(heater_stats::totals().water_l == original_totals.water_l);
  CHECK(heater_stats::totals().energy_kwh == original_totals.energy_kwh);
  CHECK(heater_stats::totals().sessions == original_totals.sessions);
  CHECK(heater_stats::settings().gas_type == heater_stats::GasType::GN);
  CHECK(heater_stats::settings().gas_tariff_per_unit == original_settings.gas_tariff);
  CHECK(heater_stats::settings().water_tariff_per_m3 == original_settings.water_tariff);
  CHECK(rinnai_storage::history.count() == 0);
  const auto initial_sync_calls = preferences.sync_calls;
  CHECK(rinnai_storage::save_totals());  // Missing archive bypasses ten-minute gate.
  CHECK(rinnai_storage::archive_saved);
  CHECK(preferences.durable.at(ARCHIVE_KEY).size() == 2624);
  CHECK(preferences.durable.at(V2_ARCHIVE_KEY).size() == 1952);
  CHECK(preferences.durable.at(OLD_ARCHIVE_KEY).size() == 528);
  CHECK(preferences.durable.at(TOTALS_KEY).size() == 32);
  CHECK(preferences.durable.at(SETTINGS_KEY).size() == 24);
  rinnai_storage::ExtendedArchive archive;
  CHECK(preferences.read(ARCHIVE_KEY, archive));
  CHECK(archive.version == 3 && archive.history.count == 0);
  CHECK(archive.periods.version == 1 && archive.periods.day_count >= 1);
  CHECK(std::memcmp(&archive.totals, &original_totals, sizeof(original_totals)) == 0);
  CHECK(preferences.sync_calls == initial_sync_calls + 1);
  CHECK(preferences.durable.at(BOOT_KEY).size() == sizeof(heater_health::Snapshot));

  reboot(preferences, 500);
  CHECK(rinnai_storage::archive_loaded);
  CHECK(rinnai_storage::archive_saved);
  CHECK(heater_stats::totals().water_l == original_totals.water_l);
  CHECK(heater_stats::totals().sessions == 5);
  CHECK(heater_stats::persist_snapshot().heating_remainder_ms == 123);
  CHECK(heater_stats::settings().gas_tariff_per_unit == original_settings.gas_tariff);
  CHECK(heater_stats::settings().water_tariff_per_m3 == original_settings.water_tariff);
  CHECK(rinnai_storage::history.count() == 0);
}

static void twenty_four_records_and_totals_survive_reboot() {
  esphome::ESPPreferences preferences;
  reboot(preferences);
  CHECK(rinnai_storage::configure({heater_stats::GasType::GLP, 4.5, 2.25}));
  for (uint32_t sequence = 1; sequence <= 30; ++sequence) {
    const auto closed = session(sequence);
    esphome::test_millis = closed.end_ms + 3000;
    CHECK(heater_stats::set_counters({sequence * 10.0, sequence * 0.125, sequence * 55, sequence}));
    CHECK(rinnai_storage::history.add(closed, esphome::test_millis, EPOCH + sequence * 70));
    CHECK(rinnai_storage::save_totals(true));
  }
  CHECK(rinnai_storage::history.count() == 24);
  CHECK(rinnai_storage::history.record(0).sequence == 7);
  CHECK(rinnai_storage::history.record(23).sequence == 30);
  rinnai_storage::ExtendedArchive persisted;
  CHECK(preferences.read(ARCHIVE_KEY, persisted));
  CHECK(persisted.history.count == 24);
  CHECK(persisted.totals.sessions == 30);
  CHECK(persisted.totals.water_l == 300);
  CHECK(persisted.totals.energy_kwh == 3.75);
  CHECK(persisted.totals.heating_seconds == 1650);
  heater_stats::Persist fallback;
  CHECK(preferences.read(TOTALS_KEY, fallback));
  CHECK(std::memcmp(&fallback, &persisted.totals, sizeof(fallback)) == 0);

  reboot(preferences);
  CHECK(rinnai_storage::archive_loaded);
  CHECK(rinnai_storage::history.count() == 24);
  CHECK(heater_stats::totals().water_l == 300);
  CHECK(heater_stats::totals().energy_kwh == 3.75);
  CHECK(heater_stats::totals().heating_seconds == 1650);
  CHECK(heater_stats::totals().sessions == 30);
  for (uint32_t i = 0; i < 24; ++i) {
    const auto restored = rinnai_storage::history.record(i);
    CHECK(restored.sequence == i + 7);
    CHECK(restored.water_l == 10 && restored.energy_kwh == 0.125);
    CHECK(restored.duration_ms == 60000 && restored.heating_seconds == 55);
    CHECK(restored.complete == 1);
    CHECK(std::isnan(rinnai_storage::history.age_s(i, 1000)));
  }
  CHECK(rinnai_storage::history.age_s(0, 1000, EPOCH + 3600) == 3113);
  CHECK(heater_stats::settings().gas_type == heater_stats::GasType::GLP);
  CHECK(heater_stats::settings().gas_tariff_per_unit == 4.5);
  CHECK(heater_stats::settings().water_tariff_per_m3 == 2.25);
}

static void invalid_archive_falls_back_to_legacy() {
  rinnai_storage::ExtendedArchive valid;
  valid.totals = legacy();
  heater_history::History prior;
  CHECK(prior.add(session(5), session(5).end_ms + 3000, EPOCH));
  valid.history = prior.snapshot();
  const auto fallback = legacy();
  auto rejected = [&](rinnai_storage::ExtendedArchive bad, bool truncate = false) {
    esphome::ESPPreferences preferences;
    preferences.seed(TOTALS_KEY, fallback);
    preferences.seed(SETTINGS_KEY, settings());
    preferences.seed(ARCHIVE_KEY, bad);
    if (truncate) preferences.durable[ARCHIVE_KEY].pop_back();
    reboot(preferences);
    CHECK(!rinnai_storage::archive_loaded);
    CHECK(!rinnai_storage::archive_saved);
    CHECK(rinnai_storage::history.count() == 0);
    CHECK(heater_stats::totals().water_l == fallback.water_l);
    CHECK(heater_stats::totals().energy_kwh == fallback.energy_kwh);
    CHECK(heater_stats::totals().sessions == fallback.sessions);
    CHECK(heater_stats::settings().gas_tariff_per_unit == settings().gas_tariff);
    CHECK(rinnai_storage::save_totals());
    rinnai_storage::ExtendedArchive repaired;
    CHECK(preferences.read(ARCHIVE_KEY, repaired));
    CHECK(repaired.version == 3 && repaired.reserved == 0 && repaired.history.count == 0);
    CHECK(repaired.totals.water_l == fallback.water_l);
  };
  auto bad = valid; bad.version = 4; rejected(bad);
  bad = valid; bad.reserved = 1; rejected(bad);
  bad = valid; bad.history.count = 25; rejected(bad);
  bad = valid; bad.history.records[0].water_l = NAN; rejected(bad);
  bad = valid; bad.totals.version = 2; rejected(bad);
  bad = valid; bad.totals.water_l = NAN; rejected(bad);
  bad = valid; bad.totals.sessions = 4; rejected(bad);
  bad = valid; bad.periods.day_count = 32; rejected(bad);
  bad = valid; bad.periods.offset_seconds = 86401; rejected(bad);
  rejected(valid, true);
}

static void write_gating_sync_failure_and_recovery() {
  esphome::ESPPreferences preferences;
  preferences.seed(TOTALS_KEY, legacy());
  reboot(preferences);
  CHECK(rinnai_storage::save_totals());
  const unsigned initial_saves = preferences.save_calls;
  esphome::test_millis += 1000;
  CHECK(rinnai_storage::save_totals());
  CHECK(preferences.save_calls == initial_saves);
  CHECK(heater_stats::set_counters({220, 6, 3700, 5}));
  CHECK(rinnai_storage::save_totals());
  CHECK(preferences.save_calls == initial_saves);

  preferences.allow_sync = false;
  CHECK(!rinnai_storage::save_totals(true));
  CHECK(!rinnai_storage::storage_ok);
  CHECK(!preferences.pending.empty());
  rinnai_storage::ExtendedArchive old;
  CHECK(preferences.read(ARCHIVE_KEY, old));
  CHECK(old.totals.water_l == legacy().water_l);
  const unsigned failed_saves = preferences.save_calls;
  esphome::test_millis += 1000;
  CHECK(!rinnai_storage::save_totals());
  CHECK(preferences.save_calls == failed_saves);
  preferences.allow_sync = true;
  esphome::test_millis += 10000;
  CHECK(rinnai_storage::save_totals());
  CHECK(rinnai_storage::storage_ok);
  CHECK(preferences.pending.empty());
  CHECK(preferences.read(ARCHIVE_KEY, old));
  CHECK(old.totals.water_l == 220);
  reboot(preferences);
  CHECK(heater_stats::totals().water_l == 220);

  CHECK(heater_stats::set_counters({230, 7, 3800, 5}));
  preferences.allow_save = false;
  CHECK(!rinnai_storage::save_totals(true));
  reboot(preferences);  // Uncommitted new counters cannot alter durable bytes.
  CHECK(heater_stats::totals().water_l == 220);
}

static void version_one_archive_migration_and_invalid_extended_fallback() {
  rinnai_storage::Archive prior;
  prior.totals = legacy();
  heater_history::History history;
  CHECK(history.add(session(5), session(5).end_ms + 3000, EPOCH));
  prior.history = history.legacy_snapshot();
  esphome::ESPPreferences preferences;
  preferences.seed(OLD_ARCHIVE_KEY, prior);
  preferences.seed(TOTALS_KEY, heater_stats::Persist{});  // Older fallback must not supersede v1 archive.
  reboot(preferences);
  CHECK(rinnai_storage::archive_loaded && !rinnai_storage::archive_saved);
  CHECK(heater_stats::totals().water_l == prior.totals.water_l);
  CHECK(rinnai_storage::history.count() == 1);
  CHECK(rinnai_storage::history.record(0).sequence == 5);
  CHECK(rinnai_storage::periods.unassigned().water_l == 0);
  CHECK(rinnai_storage::periods.today(rinnai_storage::epoch_now()).water_l == 0);
  CHECK(rinnai_storage::save_totals());
  rinnai_storage::ExtendedArchive extended;
  CHECK(preferences.read(ARCHIVE_KEY, extended));
  CHECK(extended.version == 3 && extended.history.count == 1);
  CHECK(extended.totals.water_l == prior.totals.water_l);
  CHECK(preferences.durable.at(OLD_ARCHIVE_KEY).size() == sizeof(prior));

  // A corrupt v2 must fall back to the last v1 archive, retaining its records,
  // not merely its older totals-only checkpoint.
  extended.periods.version = 42;
  preferences.seed(ARCHIVE_KEY, extended);
  preferences.durable.erase(STREAM_KEY);  // Simulate loss of the newer primary too.
  reboot(preferences);
  CHECK(rinnai_storage::archive_loaded && !rinnai_storage::archive_saved);
  CHECK(rinnai_storage::history.count() == 1);
  CHECK(rinnai_storage::history.record(0).sequence == 5);
  CHECK(heater_stats::totals().sessions == 5);
  CHECK(rinnai_storage::save_totals());
  CHECK(preferences.read(ARCHIVE_KEY, extended));
  CHECK(extended.periods.version == 1);
  reboot(preferences);
  CHECK(rinnai_storage::archive_loaded && rinnai_storage::archive_saved);
  CHECK(rinnai_storage::history.count() == 1);
}

static double total_period_water(const heater_periods::Periods &periods) {
  double result = 0;
  for (size_t i = 0; i < periods.day_count(); ++i) result += periods.day(i).water_l;
  return result;
}

static void periods_preferences_and_boots_survive_together() {
  esphome::ESPPreferences preferences;
  preferences.seed(TOTALS_KEY, legacy());
  reboot(preferences);
  const auto epoch = rinnai_storage::epoch_now();
  CHECK(heater_periods::clock_valid(epoch));
  CHECK(total_period_water(rinnai_storage::periods) == 0);  // Never date legacy totals.
  CHECK(rinnai_storage::health.boot_count() == 1);
  auto options = rinnai_storage::health.settings();
  options.long_use_min = 12;
  options.continuous_flow_min = 25;
  std::memset(options.favorites[0].name, 0, sizeof(options.favorites[0].name));
  std::memcpy(options.favorites[0].name, "Banho pessoal", 13);
  options.favorites[0].temperature_c = 39;
  CHECK(rinnai_storage::configure_preferences(options));
  CHECK(preferences.durable.at(PREFERENCES_KEY).size() == sizeof(options));
  auto counters = heater_stats::totals();
  counters.water_l += 10;
  counters.energy_kwh += 0.2;
  counters.heating_seconds += 1;
  counters.sessions += 1;
  CHECK(heater_stats::set_counters(counters));
  CHECK(rinnai_storage::periods.observe(counters, epoch + 1));
  CHECK(total_period_water(rinnai_storage::periods) == 10);
  CHECK(rinnai_storage::save_totals(true));
  rinnai_storage::ExtendedArchive saved;
  CHECK(preferences.read(ARCHIVE_KEY, saved));
  CHECK(saved.periods.day_count >= 1 && saved.periods.month_count >= 1);
  CHECK(saved.totals.water_l == counters.water_l);
  double period_month_water = 0;
  for (size_t i = 0; i < saved.periods.month_count; ++i) period_month_water += saved.periods.months[i].water_l;
  CHECK(period_month_water == 10);

  reboot(preferences, 500);
  CHECK(rinnai_storage::archive_loaded);
  CHECK(total_period_water(rinnai_storage::periods) == 10);
  CHECK(heater_stats::totals().water_l == counters.water_l);
  CHECK(rinnai_storage::health.boot_count() == 2);
  CHECK(rinnai_storage::health.settings().long_use_min == 12);
  CHECK(rinnai_storage::health.settings().continuous_flow_min == 25);
  CHECK(rinnai_storage::health.settings().favorites[0].temperature_c == 39);
  CHECK(std::strcmp(rinnai_storage::health.settings().favorites[0].name, "Banho pessoal") == 0);
  CHECK(rinnai_storage::periods.today(rinnai_storage::epoch_now()).partial);
  counters.water_l += 5;
  counters.energy_kwh += 0.1;
  counters.heating_seconds += 1;
  CHECK(heater_stats::set_counters(counters));
  CHECK(rinnai_storage::periods.observe(counters, epoch + 2));
  CHECK(total_period_water(rinnai_storage::periods) == 15);
  CHECK(rinnai_storage::save_totals(true));
  reboot(preferences);
  CHECK(total_period_water(rinnai_storage::periods) == 15);
  CHECK(rinnai_storage::health.boot_count() == 3);
}

static void version_two_twelve_record_archive_migrates_without_loss() {
  rinnai_storage::ExtendedArchiveV2 prior;
  prior.totals.water_l = 120;
  prior.totals.energy_kwh = 1.5;
  prior.totals.heating_seconds = 660;
  prior.totals.sessions = 12;
  heater_history::History source;
  for (uint32_t id = 1; id <= 12; ++id)
    CHECK(source.add(session(id), session(id).end_ms + 3000, EPOCH + id * 70));
  prior.history = source.legacy_snapshot();
  // One undated older entry must remain undated after migration and sync.
  prior.history.records[0].end_epoch_s = 0;
  heater_periods::Periods old_periods;
  old_periods.seed({119, 1.4, 659, 11}, EPOCH);
  CHECK(old_periods.observe({120, 1.5, 660, 12}, EPOCH + 1));
  prior.periods = old_periods.snapshot();
  auto options = heater_health::Settings{};
  options.favorites[0].temperature_c = 37;
  options.long_use_min = 18;
  heater_health::Monitor old_boots;
  CHECK(old_boots.begin_boot(1, EPOCH - 3600, 1000));
  CHECK(old_boots.begin_boot(4, EPOCH - 1800, 1000));
  esphome::ESPPreferences preferences;
  preferences.seed(V2_ARCHIVE_KEY, prior);
  preferences.seed(TOTALS_KEY, legacy());  // Stale older checkpoints lose priority.
  preferences.seed(OLD_ARCHIVE_KEY, rinnai_storage::Archive{});
  preferences.seed(SETTINGS_KEY, settings());
  preferences.seed(PREFERENCES_KEY, options);
  preferences.seed(BOOT_KEY, old_boots.snapshot());
  reboot(preferences);
  CHECK(rinnai_storage::archive_loaded && !rinnai_storage::archive_saved);
  CHECK(rinnai_storage::history.count() == 12);
  CHECK(heater_stats::totals().water_l == 120 && heater_stats::totals().sessions == 12);
  CHECK(total_period_water(rinnai_storage::periods) == 1);
  CHECK(rinnai_storage::health.settings().favorites[0].temperature_c == 37);
  CHECK(rinnai_storage::health.settings().long_use_min == 18);
  CHECK(rinnai_storage::health.boot_count() == 3 && rinnai_storage::health.crash_count() == 1);
  CHECK(heater_stats::settings().gas_tariff_per_unit == settings().gas_tariff);
  for (size_t i = 0; i < 12; ++i) {
    const auto entry = rinnai_storage::history.record(i);
    CHECK(entry.sequence == i + 1);
    CHECK(entry.start_epoch_s == 0);
    CHECK(entry.end_epoch_s == prior.history.records[i].end_epoch_s);
    CHECK(entry.water_l == prior.history.records[i].water_l && entry.energy_kwh == prior.history.records[i].energy_kwh);
  }
  CHECK(std::isnan(rinnai_storage::history.age_s(0, 1000, EPOCH + 3600)));
  CHECK(!rinnai_storage::history.synchronize_clock(1000, EPOCH + 3600));
  CHECK(rinnai_storage::save_totals());
  CHECK(preferences.durable.at(ARCHIVE_KEY).size() == 2624);
  CHECK(preferences.durable.at(V2_ARCHIVE_KEY).size() == 1952);
  reboot(preferences);
  CHECK(rinnai_storage::archive_loaded && rinnai_storage::archive_saved);
  CHECK(rinnai_storage::history.count() == 12 && total_period_water(rinnai_storage::periods) == 1);

  for (uint32_t id = 13; id <= 30; ++id) {
    const auto closed = session(id);
    esphome::test_millis = closed.end_ms + 3000;
    CHECK(heater_stats::set_counters({id * 10.0, id * 0.125, id * 55, id}));
    CHECK(rinnai_storage::history.add(closed, esphome::test_millis, EPOCH + id * 70));
    CHECK(rinnai_storage::save_totals(true));
  }
  CHECK(rinnai_storage::history.count() == 24);
  CHECK(rinnai_storage::history.record(0).sequence == 7 && rinnai_storage::history.record(23).sequence == 30);
  CHECK(rinnai_storage::history.record(0).start_epoch_s == 0);  // Old record remains honest.
  CHECK(rinnai_storage::history.record(6).start_epoch_s != 0);  // New id13 has real start.
  rinnai_storage::ExtendedArchiveV2 rollback;
  CHECK(preferences.read(V2_ARCHIVE_KEY, rollback));
  CHECK(rollback.version == 2 && rollback.history.count == 12);
  CHECK(rollback.history.records[0].sequence == 19 && rollback.history.records[11].sequence == 30);
  CHECK(rollback.totals.water_l == 300 && rollback.totals.sessions == 30);
  heater_history::History compatible;
  CHECK(compatible.restore(rollback.history));
  CHECK(compatible.count() == 12 && compatible.record(0).sequence == 19);

  reboot(preferences);
  CHECK(rinnai_storage::history.count() == 24);
  CHECK(rinnai_storage::history.record(23).start_epoch_s != 0);
  CHECK(heater_stats::totals().water_l == 300 && total_period_water(rinnai_storage::periods) == 1);
  // Missing newest format falls back to the maintained v2 checkpoint.
  preferences.durable.erase(ARCHIVE_KEY);
  preferences.durable.erase(STREAM_KEY);
  reboot(preferences);
  CHECK(rinnai_storage::archive_loaded && !rinnai_storage::archive_saved);
  CHECK(rinnai_storage::history.count() == 12 && rinnai_storage::history.record(0).sequence == 19);
  CHECK(heater_stats::totals().water_l == 300 && total_period_water(rinnai_storage::periods) == 1);
  CHECK(rinnai_storage::health.settings().favorites[0].temperature_c == 37);
}

static void newer_rollback_archive_merges_without_erasing_real_starts() {
  esphome::ESPPreferences preferences;
  reboot(preferences);
  for (uint32_t id = 1; id <= 24; ++id) {
    const auto ended = session(id);
    esphome::test_millis = ended.end_ms + 3000;
    CHECK(heater_stats::set_counters({id * 10.0, id * 0.125, id * 55, id}));
    CHECK(rinnai_storage::history.add(ended, esphome::test_millis, EPOCH + id * 70));
  }
  CHECK(rinnai_storage::save_totals(true));
  rinnai_storage::ExtendedArchive original;
  rinnai_storage::ExtendedArchiveV2 rollback;
  CHECK(preferences.read(ARCHIVE_KEY, original));
  CHECK(preferences.read(V2_ARCHIVE_KEY, rollback));
  const auto known_start = original.history.records[2].start_epoch_s;
  CHECK(known_start != 0 && original.history.records[2].sequence == 3);
  heater_history::History old_image;
  CHECK(old_image.restore(rollback.history));
  for (uint32_t id = 25; id <= 26; ++id)
    CHECK(old_image.add(session(id), session(id).end_ms + 3000, EPOCH + id * 70));
  rollback.history = old_image.legacy_snapshot();
  rollback.totals.water_l = 260;
  rollback.totals.energy_kwh = 3.25;
  rollback.totals.heating_seconds = 1430;
  rollback.totals.sessions = 26;
  heater_periods::Periods newer_periods;
  newer_periods.seed({240, 3, 1320, 24}, EPOCH);
  CHECK(newer_periods.observe({260, 3.25, 1430, 26}, EPOCH + 1));
  rollback.periods = newer_periods.snapshot();
  preferences.seed(V2_ARCHIVE_KEY, rollback);
  reboot(preferences);
  CHECK(rinnai_storage::archive_loaded && !rinnai_storage::archive_saved);
  CHECK(heater_stats::totals().water_l == 260 && heater_stats::totals().sessions == 26);
  CHECK(total_period_water(rinnai_storage::periods) == 20);
  CHECK(rinnai_storage::history.count() == 24);
  CHECK(rinnai_storage::history.record(0).sequence == 3);
  CHECK(rinnai_storage::history.record(0).start_epoch_s == known_start);
  CHECK(rinnai_storage::history.record(22).sequence == 25 && rinnai_storage::history.record(22).start_epoch_s == 0);
  CHECK(rinnai_storage::history.record(23).sequence == 26 && rinnai_storage::history.record(23).start_epoch_s == 0);
  CHECK(rinnai_storage::save_totals());
  CHECK(preferences.read(ARCHIVE_KEY, original));
  CHECK(original.history.count == 24 && original.totals.sessions == 26);
  CHECK(original.history.records[0].start_epoch_s == known_start);
  // An older v2 must never supersede the now-current v3 file.
  rollback.totals.water_l = 250;
  rollback.totals.energy_kwh = 3.125;
  rollback.totals.heating_seconds = 1375;
  rollback.totals.sessions = 25;
  rollback.history.count = 11;
  rollback.history.records[11] = heater_history::LegacyRecord{};
  preferences.seed(V2_ARCHIVE_KEY, rollback);
  reboot(preferences);
  CHECK(rinnai_storage::archive_loaded && rinnai_storage::archive_saved);
  CHECK(heater_stats::totals().sessions == 26 && heater_stats::totals().water_l == 260);
  CHECK(rinnai_storage::history.record(0).start_epoch_s == known_start);
}

static void stream_v4_migration_and_durable_export() {
  esphome::ESPPreferences preferences;
  rinnai_storage::ExtendedArchive prior;
  prior.totals = {240, 3, 1, 1320, 24, 0, 0};
  heater_history::History source;
  for (uint32_t sequence = 1; sequence <= 24; ++sequence)
    CHECK(source.add(session(sequence), session(sequence).end_ms + 3000, EPOCH + sequence * 70));
  prior.history = source.snapshot();
  prior.history.records[0].start_epoch_s = 0;  // Preserve honest legacy data.
  preferences.seed(ARCHIVE_KEY, prior);
  preferences.seed(SETTINGS_KEY, settings());
  reboot(preferences);
  CHECK(rinnai_storage::archive_loaded && !rinnai_storage::archive_saved);
  CHECK(!rinnai_storage::stream_ready);
  rinnai_sessions::Snapshot initial;
  rinnai_storage::copy_sessions_snapshot(initial);
  CHECK(!initial.ready);
  CHECK(initial.history.count == 0);
  CHECK(rinnai_storage::save_totals(true));
  CHECK(preferences.durable.at(STREAM_KEY).size() == sizeof(rinnai_storage::StreamArchive));
  rinnai_sessions::Snapshot exported;
  rinnai_storage::copy_sessions_snapshot(exported);
  CHECK(exported.ready && exported.storage_ok);
  CHECK(exported.history.count == 24 && exported.total_sessions == 24);
  CHECK(std::memcmp(&exported.history, &prior.history, sizeof(prior.history)) == 0);
  CHECK(exported.settings.gas_tariff_per_unit == settings().gas_tariff);
  const auto identity = exported.stream_id;
  CHECK(rinnai_storage::valid_stream_id(identity));
  reboot(preferences);
  rinnai_storage::copy_sessions_snapshot(exported);
  CHECK(exported.ready && exported.stream_id == identity && exported.total_sessions == 24);
  CHECK(exported.history.records[0].start_epoch_s == 0);
  for (unsigned repetition = 0; repetition < 10; ++repetition) {
    rinnai_sessions::Snapshot repeated;
    rinnai_storage::copy_sessions_snapshot(repeated);
    CHECK(repeated.stream_id == identity);
    CHECK(std::memcmp(&repeated.history, &exported.history, sizeof(repeated.history)) == 0);
  }

  const auto ended = session(25);
  esphome::test_millis = ended.end_ms + 3000;
  CHECK(heater_stats::set_counters({250, 3.125, 1375, 25}));
  CHECK(rinnai_storage::history.add(ended, esphome::test_millis, EPOCH + 1750));
  preferences.allow_sync = false;
  CHECK(!rinnai_storage::save_totals(true));
  rinnai_sessions::Snapshot uncommitted;
  rinnai_storage::copy_sessions_snapshot(uncommitted);
  CHECK(!uncommitted.storage_ok && uncommitted.history.count == 0);
  CHECK(rinnai_storage::extended_last_saved.totals.sessions == 24);
  reboot(preferences);  // Pending V4 is discarded by power loss.
  rinnai_storage::copy_sessions_snapshot(exported);
  CHECK(exported.ready && exported.storage_ok && exported.stream_id == identity);
  CHECK(exported.total_sessions == 24 && exported.history.count == 24);
  CHECK(exported.history.records[23].sequence == 24);
  CHECK(heater_stats::set_counters({250, 3.125, 1375, 25}));
  CHECK(rinnai_storage::history.add(ended, ended.end_ms + 3000, EPOCH + 1750));
  CHECK(rinnai_storage::save_totals(true));
  rinnai_storage::copy_sessions_snapshot(exported);
  CHECK(exported.total_sessions == 25 && exported.history.records[23].sequence == 25);
  CHECK(exported.stream_id == identity);
}

static void stream_reset_and_failed_first_commit() {
  esphome::ESPPreferences preferences;
  preferences.seed(TOTALS_KEY, legacy());
  reboot(preferences);
  const auto never_published = rinnai_storage::stream_id;
  preferences.allow_save = false;
  CHECK(!rinnai_storage::save_totals(true));
  rinnai_sessions::Snapshot snapshot;
  rinnai_storage::copy_sessions_snapshot(snapshot);
  CHECK(!snapshot.ready && !snapshot.storage_ok);
  reboot(preferences);
  CHECK(rinnai_storage::stream_id != never_published);
  CHECK(rinnai_storage::save_totals(true));
  rinnai_storage::copy_sessions_snapshot(snapshot);
  CHECK(snapshot.ready && snapshot.storage_ok);
  const auto first = snapshot.stream_id;
  preferences.durable.erase(STREAM_KEY);  // V3 fallback keeps counters, new identity.
  reboot(preferences);
  CHECK(!rinnai_storage::stream_ready && !rinnai_storage::archive_saved);
  CHECK(rinnai_storage::stream_id != first);
  CHECK(heater_stats::totals().sessions == 5);
  CHECK(rinnai_storage::save_totals());
  rinnai_storage::copy_sessions_snapshot(snapshot);
  const auto second = snapshot.stream_id;
  CHECK(second != first && snapshot.total_sessions == 5);
  preferences.durable.clear();  // Complete NVS reset is another stream.
  reboot(preferences);
  CHECK(rinnai_storage::save_totals());
  rinnai_storage::copy_sessions_snapshot(snapshot);
  CHECK(snapshot.ready && snapshot.stream_id != first && snapshot.stream_id != second);
  CHECK(snapshot.total_sessions == 0 && snapshot.history.count == 0);

  preferences.durable.clear();
  const auto entropy = esphome::test_random_state;
  esphome::test_random_state = 0;
  reboot(preferences);
  CHECK(!rinnai_storage::session_storage_ok);
  CHECK(!rinnai_storage::save_totals());
  rinnai_storage::copy_sessions_snapshot(snapshot);
  CHECK(!snapshot.ready && !snapshot.storage_ok);
  esphome::test_random_state = entropy;
}

static void stream_clock_refinement_and_exhaustion() {
  esphome::ESPPreferences preferences;
  reboot(preferences);
  const auto ended = session(1);
  esphome::test_millis = ended.end_ms + 3000;
  CHECK(heater_stats::set_counters({10, .125, 55, 1}));
  CHECK(rinnai_storage::history.add(ended, esphome::test_millis, 0));
  CHECK(rinnai_storage::save_totals(true));
  rinnai_sessions::Snapshot before, after;
  rinnai_storage::copy_sessions_snapshot(before);
  CHECK(before.ready && before.history.records[0].start_epoch_s == 0);
  CHECK(rinnai_storage::history.synchronize_clock(esphome::test_millis, EPOCH));
  rinnai_storage::copy_sessions_snapshot(after);
  CHECK(after.history.records[0].start_epoch_s == 0);  // No unsynced RAM leak.
  CHECK(rinnai_storage::save_totals(true));
  rinnai_storage::copy_sessions_snapshot(after);
  CHECK(after.stream_id == before.stream_id);
  CHECK(after.history.records[0].sequence == before.history.records[0].sequence);
  CHECK(after.history.records[0].start_epoch_s != 0 && after.history.records[0].end_epoch_s != 0);

  preferences.durable.clear();
  reboot(preferences);
  auto maximum = session(UINT32_MAX);
  esphome::test_millis = maximum.end_ms + 3000;
  CHECK(heater_stats::set_counters({10, .125, 55, UINT32_MAX}));
  CHECK(rinnai_storage::history.add(maximum, esphome::test_millis, EPOCH));
  CHECK(rinnai_storage::save_totals(true));
  rinnai_storage::copy_sessions_snapshot(after);
  CHECK(after.ready && after.total_sessions == UINT32_MAX);
  CHECK(after.history.count == 1 && after.history.records[0].sequence == UINT32_MAX);
  CHECK(!rinnai_storage::history.add(maximum, esphome::test_millis, EPOCH));
  CHECK(rinnai_storage::history.count() == 1);
}

static void stream_v3_rollback_extension_and_corrupt_primary() {
  esphome::ESPPreferences preferences;
  reboot(preferences);
  for (uint32_t sequence = 1; sequence <= 3; ++sequence) {
    const auto ended = session(sequence);
    esphome::test_millis = ended.end_ms + 3000;
    CHECK(heater_stats::set_counters({sequence * 10.0, sequence * .125, sequence * 55, sequence}));
    CHECK(rinnai_storage::history.add(ended, esphome::test_millis, EPOCH + sequence * 70));
  }
  CHECK(rinnai_storage::save_totals(true));
  const auto original_stream = rinnai_storage::stream_id;
  const auto known_start = rinnai_storage::history.record(0).start_epoch_s;
  rinnai_storage::ExtendedArchive newer;
  CHECK(preferences.read(ARCHIVE_KEY, newer));
  heater_history::History rollback;
  CHECK(rollback.restore(newer.history));
  CHECK(rollback.add(session(4), session(4).end_ms + 3000, EPOCH + 280));
  newer.history = rollback.snapshot();
  newer.totals = {40, .5, 1, 220, 4, 0, 0};
  preferences.seed(ARCHIVE_KEY, newer);
  reboot(preferences);
  CHECK(rinnai_storage::stream_id == original_stream);
  CHECK(!rinnai_storage::archive_saved && heater_stats::totals().sessions == 4);
  CHECK(rinnai_storage::history.count() == 4 && rinnai_storage::history.record(0).start_epoch_s == known_start);
  rinnai_sessions::Snapshot exported;
  rinnai_storage::copy_sessions_snapshot(exported);
  CHECK(!exported.ready);
  CHECK(rinnai_storage::save_totals());
  rinnai_storage::copy_sessions_snapshot(exported);
  CHECK(exported.ready && exported.stream_id == original_stream && exported.total_sessions == 4);

  rinnai_storage::StreamArchive corrupted;
  CHECK(preferences.read(STREAM_KEY, corrupted));
  corrupted.reserved = 1;
  preferences.seed(STREAM_KEY, corrupted);
  reboot(preferences);
  CHECK(rinnai_storage::stream_id != original_stream);
  CHECK(rinnai_storage::history.count() == 4 && heater_stats::totals().sessions == 4);
  CHECK(!rinnai_storage::stream_ready && !rinnai_storage::archive_saved);
  CHECK(rinnai_storage::save_totals());
  const auto restored_stream = rinnai_storage::stream_id;
  CHECK(preferences.read(ARCHIVE_KEY, newer));
  newer.history.records[3].water_l += 1;  // A checkpoint fork cannot reuse old IDs.
  newer.totals.water_l += 1;
  preferences.seed(ARCHIVE_KEY, newer);
  reboot(preferences);
  CHECK(rinnai_storage::stream_id != restored_stream);
  CHECK(!rinnai_storage::stream_ready);
  CHECK(rinnai_storage::save_totals());
  rinnai_storage::copy_sessions_snapshot(exported);
  CHECK(exported.ready && exported.history.records[3].water_l == 11);
}

int main() {
  legacy_migration_and_settings_survive();
  twenty_four_records_and_totals_survive_reboot();
  invalid_archive_falls_back_to_legacy();
  write_gating_sync_failure_and_recovery();
  version_one_archive_migration_and_invalid_extended_fallback();
  periods_preferences_and_boots_survive_together();
  version_two_twelve_record_archive_migrates_without_loss();
  newer_rollback_archive_merges_without_erasing_real_starts();
  stream_v4_migration_and_durable_export();
  stream_reset_and_failed_first_commit();
  stream_clock_refinement_and_exhaustion();
  stream_v3_rollback_extension_and_corrupt_primary();
  std::cout << "PASS: " << checks << " assertions in 12 heater-storage integration cases\n";
}
