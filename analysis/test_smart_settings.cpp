#include "../esphome/heater_storage.h"

#include <cstdlib>
#include <iostream>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; std::exit(EXIT_FAILURE); } } while (false)
static constexpr uint32_t TOTALS_KEY = 0x6EA91C52;
static constexpr uint32_t TARIFFS_KEY = 0x6EA91C53;
static constexpr uint32_t ARCHIVE_KEY = 0x6EA91C54;
static constexpr uint32_t EXTENDED_KEY = 0x6EA91C5A;
static constexpr uint32_t PREFERENCES_KEY = 0x6EA91C56;
static constexpr uint32_t HEALTH_KEY = 0x6EA91C57;

static void reboot(esphome::ESPPreferences &preferences, uint32_t reason = 3) {
  preferences.pending.clear();
  preferences.allow_save = preferences.allow_sync = true;
  esphome::global_preferences = &preferences;
  esphome::test_millis = 1000;
  heater_stats::detail::accounting = heater_stats::detail::Accounting{};
  rinnai_storage::initialized = false;
  rinnai_storage::last_attempt_ms = 0;
  rinnai_storage::begin(reason);
}

static void favorite(heater_health::Favorite &target, const char *value, int32_t temperature) {
  CHECK(std::strlen(value) < sizeof(target.name));
  std::memset(target.name, 0, sizeof(target.name));
  std::memcpy(target.name, value, std::strlen(value));
  target.temperature_c = temperature;
}

static heater_health::Settings customized() {
  heater_health::Settings settings;
  settings.enabled_mask = heater_health::LONG_USE | heater_health::HEATER_ERROR | heater_health::OFFLINE;
  settings.long_use_min = 12;
  settings.continuous_flow_min = 25;
  settings.offline_seconds = 45;
  favorite(settings.favorites[0], "Meu banho", 38);
  favorite(settings.favorites[1], "Cozinha", 42);
  favorite(settings.favorites[2], "Banho da m\xC3\xA3" "e", 40);
  return settings;
}

static void same_preferences(const heater_health::Settings &expected) {
  const auto actual = rinnai_storage::health.settings();
  CHECK(std::memcmp(&actual, &expected, sizeof(expected)) == 0);
}

static void migration_and_preferences_survive_reboot() {
  esphome::ESPPreferences preferences;
  heater_stats::Persist legacy;
  legacy.water_l = 214.5375;
  legacy.energy_kwh = 4.10165;
  legacy.heating_seconds = 1744;
  legacy.sessions = 5;
  preferences.seed(TOTALS_KEY, legacy);
  rinnai_storage::SettingsPersist tariffs;
  tariffs.gas_type = 0;
  tariffs.gas_tariff = 8.25;
  tariffs.water_tariff = 10.5;
  preferences.seed(TARIFFS_KEY, tariffs);
  reboot(preferences, 1);
  CHECK(rinnai_storage::health.boot_count() == 1);
  CHECK(rinnai_storage::health.boot(0).reason_code == 1);
  same_preferences(heater_health::Settings{});
  CHECK(heater_stats::totals().water_l == legacy.water_l && heater_stats::totals().sessions == 5);
  CHECK(heater_stats::settings().gas_tariff_per_unit == 8.25);
  CHECK(heater_stats::settings().water_tariff_per_m3 == 10.5);
  const auto settings = customized();
  CHECK(rinnai_storage::configure_preferences(settings));
  CHECK(rinnai_storage::preferences_storage_ok);
  same_preferences(settings);
  heater_health::Settings stored;
  CHECK(preferences.read(PREFERENCES_KEY, stored));
  CHECK(std::memcmp(&stored, &settings, sizeof(stored)) == 0);
  CHECK(preferences.durable.at(PREFERENCES_KEY).size() == 132);
  CHECK(rinnai_storage::save_totals(true));
  CHECK(preferences.durable.at(EXTENDED_KEY).size() == 2624);
  CHECK(preferences.durable.at(ARCHIVE_KEY).size() == 528);
  CHECK(preferences.durable.at(TOTALS_KEY).size() == 32);
  CHECK(preferences.durable.at(HEALTH_KEY).size() == 208);
  reboot(preferences, 4);
  CHECK(rinnai_storage::archive_loaded);
  CHECK(rinnai_storage::health.boot_count() == 2 && rinnai_storage::health.crash_count() == 1);
  same_preferences(settings);
  CHECK(heater_stats::totals().water_l == legacy.water_l);
  CHECK(heater_stats::totals().sessions == 5);
  CHECK(heater_stats::settings().gas_tariff_per_unit == 8.25);
  CHECK(heater_stats::settings().water_tariff_per_m3 == 10.5);
  heater_health::Snapshot boots;
  CHECK(preferences.read(HEALTH_KEY, boots));
  CHECK(boots.boot_count == 2 && boots.count == 2 && boots.crash_count == 1);
}

static void invalid_requests_have_no_effect() {
  esphome::ESPPreferences preferences;
  reboot(preferences);
  const auto original = customized();
  CHECK(rinnai_storage::configure_preferences(original));
  const auto durable = preferences.durable;
  const auto pending = preferences.pending;
  const auto saves = preferences.save_calls, syncs = preferences.sync_calls;
  for (unsigned i = 0; i < 11; ++i) {
    auto bad = original;
    switch (i) {
      case 0: bad.version = 2; break;
      case 1: bad.enabled_mask = 256; break;
      case 2: bad.long_use_min = 0; break;
      case 3: bad.continuous_flow_min = 181; break;
      case 4: bad.offline_seconds = 9; break;
      case 5: bad.offline_seconds = 601; break;
      case 6: bad.favorites[0].temperature_c = 34; break;
      case 7: bad.favorites[2].temperature_c = 51; break;
      case 8: bad.favorites[1].name[0] = '\0'; break;
      case 9: std::memset(bad.favorites[1].name, 'x', 32); break;
      default: bad.favorites[1].name[0] = '\n'; break;
    }
    CHECK(!rinnai_storage::configure_preferences(bad));
    same_preferences(original);
    CHECK(preferences.durable == durable);
    CHECK(preferences.pending == pending);
    CHECK(preferences.save_calls == saves && preferences.sync_calls == syncs);
  }
}

static void failed_save_and_sync_preserve_old_configuration() {
  esphome::ESPPreferences preferences;
  reboot(preferences);
  const auto original = customized();
  CHECK(rinnai_storage::configure_preferences(original));
  auto candidate = original;
  favorite(candidate.favorites[0], "Ajuste rejeitado", 49);
  candidate.long_use_min = 99;
  preferences.allow_save = false;
  CHECK(!rinnai_storage::configure_preferences(candidate));
  CHECK(!rinnai_storage::preferences_storage_ok);
  same_preferences(original);
  heater_health::Settings durable;
  CHECK(preferences.read(PREFERENCES_KEY, durable));
  CHECK(std::memcmp(&durable, &original, sizeof(durable)) == 0);
  reboot(preferences);
  same_preferences(original);

  preferences.allow_sync = false;
  CHECK(!rinnai_storage::configure_preferences(candidate));
  CHECK(!rinnai_storage::preferences_storage_ok);
  same_preferences(original);
  CHECK(preferences.read(PREFERENCES_KEY, durable));
  CHECK(std::memcmp(&durable, &original, sizeof(durable)) == 0);
  const auto pending = preferences.pending.find(PREFERENCES_KEY);
  CHECK(pending != preferences.pending.end() && pending->second.size() == sizeof(original));
  CHECK(std::memcmp(pending->second.data(), &original, sizeof(original)) == 0);
  preferences.allow_sync = true;
  CHECK(rinnai_storage::save_totals(true));  // Unrelated successful sync cannot commit rejected settings.
  CHECK(preferences.read(PREFERENCES_KEY, durable));
  CHECK(std::memcmp(&durable, &original, sizeof(durable)) == 0);
  reboot(preferences);
  same_preferences(original);
  CHECK(rinnai_storage::configure_preferences(candidate));
  same_preferences(candidate);
  CHECK(rinnai_storage::preferences_storage_ok);
  reboot(preferences);
  same_preferences(candidate);
}

static void invalid_persisted_preferences_fall_back_to_defaults() {
  const auto defaults = heater_health::Settings{};
  for (unsigned i = 0; i < 4; ++i) {
    esphome::ESPPreferences preferences;
    auto bad = customized();
    if (i == 0) bad.version = 0;
    if (i == 1) bad.favorites[0].temperature_c = 99;
    if (i == 2) bad.offline_seconds = 0;
    preferences.seed(PREFERENCES_KEY, bad);
    if (i == 3) preferences.durable[PREFERENCES_KEY].pop_back();
    reboot(preferences);
    same_preferences(defaults);
    CHECK(rinnai_storage::health.boot_count() == 1);
  }
  esphome::ESPPreferences preferences;
  esphome::global_preferences = &preferences;
  rinnai_storage::initialized = false;
  const auto previous = rinnai_storage::health.settings();
  CHECK(!rinnai_storage::configure_preferences(customized()));
  same_preferences(previous);
  CHECK(preferences.save_calls == 0 && preferences.sync_calls == 0);
}

int main() {
  migration_and_preferences_survive_reboot();
  invalid_requests_have_no_effect();
  failed_save_and_sync_preserve_old_configuration();
  invalid_persisted_preferences_fall_back_to_defaults();
  std::cout << "PASS: " << checks << " checks across 4 smart-settings integration scenarios\n";
}
