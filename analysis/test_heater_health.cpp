#include <cstdint>
static uint32_t fake_ms = 0;
#define HEATER_STATS_MILLIS() fake_ms
#include "../esphome/heater_health.h"

#include <cstdlib>
#include <iostream>

using heater_health::Monitor;
using heater_health::Metrics;
using heater_health::Settings;
using heater_health::Snapshot;
static constexpr uint64_t EPOCH = 1793000000ULL;
static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; std::exit(EXIT_FAILURE); } } while (false)

static void name(heater_health::Favorite &favorite, const char *value) {
  std::memset(favorite.name, 0, sizeof(favorite.name));
  const auto length = std::strlen(value);
  CHECK(length < sizeof(favorite.name));
  std::memcpy(favorite.name, value, length);
}

static void settings_validation_and_transaction() {
  Monitor monitor;
  Settings defaults;
  CHECK(heater_health::valid_settings(defaults));
  CHECK(defaults.favorites[0].temperature_c == 38);
  CHECK(std::strcmp(defaults.favorites[2].name, "Cozinha") == 0);
  auto config = defaults;
  config.long_use_min = 1;
  config.continuous_flow_min = 180;
  config.offline_seconds = 600;
  config.enabled_mask = 0;
  name(config.favorites[0], "Banho quente");
  config.favorites[0].temperature_c = 50;
  config.favorites[1].temperature_c = 35;
  CHECK(monitor.configure(config));
  CHECK(monitor.settings().long_use_min == 1);
  for (unsigned i = 0; i < 12; ++i) {
    auto bad = config;
    switch (i) {
      case 0: bad.version = 2; break;
      case 1: bad.reserved = 1; break;
      case 2: bad.enabled_mask = 16; break;
      case 3: bad.long_use_min = 0; break;
      case 4: bad.long_use_min = 181; break;
      case 5: bad.continuous_flow_min = 0; break;
      case 6: bad.continuous_flow_min = 181; break;
      case 7: bad.offline_seconds = 9; break;
      case 8: bad.offline_seconds = 601; break;
      case 9: bad.favorites[0].temperature_c = 34; break;
      case 10: bad.favorites[1].temperature_c = 51; break;
      default: bad.favorites[2].name[0] = '\0'; break;
    }
    CHECK(!heater_health::valid_settings(bad));
    CHECK(!monitor.configure(bad));
    const auto retained = monitor.settings();
    CHECK(std::memcmp(&retained, &config, sizeof(config)) == 0);
  }
}

static void favorite_utf8_and_lengths() {
  Settings config;
  name(config.favorites[0], "Banho da m\xC3\xA3" "e");
  CHECK(heater_health::valid_settings(config));
  name(config.favorites[0], "\xE2\x98\x85 Cozinha");
  CHECK(heater_health::valid_settings(config));
  name(config.favorites[0], "\xF0\x9F\x92\xA7 Banho");
  CHECK(heater_health::valid_settings(config));
  name(config.favorites[0], "1234567890123456789012345678901");
  CHECK(heater_health::valid_settings(config));
  std::memset(config.favorites[0].name, 'a', 32);
  CHECK(!heater_health::valid_settings(config));
  const char *invalid[] = {"", "    ", "a\nb", "a\tb", "a\x7Fz", "\xC0\xAF", "\x80",
    "\xC2", "\xE0\x80\x80", "\xED\xA0\x80", "\xF0\x80\x80\x80", "\xF4\x90\x80\x80",
    "\xF5\x80\x80\x80", "\xE2\x28\xA1", "\xF0\x9F\x28\xA7"};
  for (const auto value : invalid) {
    name(config.favorites[0], value);
    CHECK(!heater_health::valid_settings(config));
  }
  // Names are data, not HTML: downstream JSON escaping/textContent is required.
  name(config.favorites[0], "<Banho> \"Pessoa\"");
  CHECK(heater_health::valid_settings(config));
}

static void boots_ring_and_snapshot_restore() {
  Monitor monitor;
  CHECK(monitor.boot_count() == 0 && monitor.crash_count() == 0 && monitor.boot_size() == 0);
  uint32_t crashes = 0;
  for (uint32_t i = 1; i <= 25; ++i) {
    const uint32_t reason = i % 16;
    if (heater_health::is_crash_reason(reason)) ++crashes;
    CHECK(monitor.begin_boot(reason, EPOCH + i * 100, 2000));
    CHECK(monitor.boot_count() == i);
    CHECK(monitor.crash_count() == crashes);
    CHECK(monitor.boot_size() == (i < 12 ? i : 12));
    CHECK(monitor.boot(monitor.boot_size() - 1).sequence == i);
    CHECK(monitor.boot(monitor.boot_size() - 1).epoch_s == EPOCH + i * 100 - 2);
  }
  const auto saved = monitor.snapshot();
  CHECK(saved.count == 12 && saved.boot_count == 25 && saved.crash_count == crashes);
  for (uint32_t i = 0; i < 12; ++i) {
    CHECK(saved.boots[i].sequence == i + 14);
    CHECK(saved.boots[i].reason_code == (i + 14) % 16);
  }
  Monitor restored;
  CHECK(restored.restore(saved));
  CHECK(restored.boot_count() == 25 && restored.boot_size() == 12);
  CHECK(!restored.synchronize_clock(1000, EPOCH + 5000));
  CHECK(restored.begin_boot(3, EPOCH + 5000, 3000));
  CHECK(restored.boot_count() == 26 && restored.crash_count() == crashes);
  CHECK(restored.boot(0).sequence == 15 && restored.boot(11).sequence == 26);
  CHECK(restored.boot(11).epoch_s == EPOCH + 4997);
  CHECK(restored.boot(12).sequence == 0);
  CHECK(!restored.begin_boot(256, EPOCH, 100));
  CHECK(restored.boot_count() == 26);
  CHECK(heater_health::is_crash_reason(4) && heater_health::is_crash_reason(7) && heater_health::is_crash_reason(15));
  CHECK(!heater_health::is_crash_reason(3) && !heater_health::is_crash_reason(9));
}

static void clocks_and_corrupt_snapshots() {
  Monitor monitor;
  CHECK(monitor.begin_boot(1, 0, 2000));
  CHECK(monitor.boot(0).epoch_s == 0);
  CHECK(!monitor.synchronize_clock(7000, 100));
  const auto old_unknown = monitor.snapshot();
  Monitor restored;
  CHECK(restored.restore(old_unknown));
  CHECK(!restored.synchronize_clock(8000, EPOCH));
  CHECK(restored.begin_boot(4, 0, 3000));
  CHECK(restored.synchronize_clock(8000, EPOCH));
  CHECK(restored.boot(0).epoch_s == 0);
  CHECK(restored.boot(1).epoch_s == EPOCH - 8);
  CHECK(!restored.synchronize_clock(9000, EPOCH + 1));
  const auto valid = restored.snapshot();
  for (unsigned i = 0; i < 10; ++i) {
    auto bad = valid;
    switch (i) {
      case 0: bad.version = 0; break;
      case 1: bad.reserved = 1; break;
      case 2: bad.count = 13; break;
      case 3: bad.boot_count = 0; break;
      case 4: bad.crash_count = 3; break;
      case 5: bad.crash_count = 0; break;
      case 6: bad.boots[0].sequence = 2; break;
      case 7: bad.boots[1].epoch_s = 1; break;
      case 8: bad.boots[1].reason_code = 256; break;
      default: bad.boots[4].sequence = 99; break;
    }
    CHECK(!restored.restore(bad));
    const auto unchanged = restored.snapshot();
    CHECK(std::memcmp(&unchanged, &valid, sizeof(valid)) == 0);
  }
  Monitor empty;
  CHECK(empty.restore(Snapshot{}));
  // The first representable date cannot be backdated before the allowed epoch.
  CHECK(empty.begin_boot(0, heater_health::MIN_VALID_EPOCH_S, 10000));
  CHECK(empty.boot(0).epoch_s == 0);
  CHECK(empty.synchronize_clock(10000, heater_health::MIN_VALID_EPOCH_S + 10));
  CHECK(empty.boot(0).epoch_s == heater_health::MIN_VALID_EPOCH_S);
}

static Metrics water(uint32_t duration_ms) {
  Metrics metrics;
  metrics.online = metrics.ever_seen = metrics.error_known = metrics.flow_fresh = true;
  metrics.session.active = true;
  metrics.session.duration_ms = duration_ms;
  metrics.session.heating_seconds = duration_ms / 1000;
  return metrics;
}

static void alert_thresholds_recovery_and_masks() {
  Monitor monitor;
  CHECK(monitor.begin_boot(1, EPOCH, 1000));
  auto metrics = water(899999);
  auto alerts = monitor.update(10000, metrics);
  CHECK(!alerts.long_use && !alerts.continuous_flow && alerts.active_count == 0);
  metrics.session.duration_ms = 900000;
  alerts = monitor.update(11000, metrics);
  CHECK(alerts.long_use && !alerts.continuous_flow && alerts.active_count == 1);
  metrics.session.duration_ms = 1200000;
  alerts = monitor.update(12000, metrics);
  CHECK(alerts.long_use && alerts.continuous_flow && alerts.active_count == 2);
  metrics.session.heating_seconds = 0;
  alerts = monitor.update(13000, metrics);
  CHECK(!alerts.long_use && alerts.continuous_flow);
  metrics.flow_fresh = false;
  alerts = monitor.update(14000, metrics);
  CHECK(!alerts.long_use && !alerts.continuous_flow);
  metrics.flow_fresh = true;
  metrics.session.active = false;
  alerts = monitor.update(15000, metrics);
  CHECK(alerts.active_count == 0);
  metrics = water(1200000);
  metrics.error_code = 11;
  alerts = monitor.update(16000, metrics);
  CHECK(alerts.active_count == 3 && alerts.heater_error && !alerts.heater_error_stale);
  metrics.error_known = false;
  metrics.error_code = 0;  // An absent error reading must not establish recovery.
  alerts = monitor.update(17000, metrics);
  CHECK(alerts.heater_error && alerts.heater_error_stale);
  metrics.error_known = true;
  alerts = monitor.update(18000, metrics);
  CHECK(!alerts.heater_error && !alerts.heater_error_stale);
  auto settings = monitor.settings();
  settings.enabled_mask = heater_health::CONTINUOUS_FLOW;
  CHECK(monitor.configure(settings));
  metrics.error_code = 12;
  alerts = monitor.update(19000, metrics);
  CHECK(alerts.active_count == 1 && alerts.continuous_flow && !alerts.heater_error && !alerts.long_use);
  settings.enabled_mask = 0;
  CHECK(monitor.configure(settings));
  CHECK(monitor.update(20000, metrics).active_count == 0);
  settings.enabled_mask = heater_health::HEATER_ERROR;
  CHECK(monitor.configure(settings));
  metrics.error_known = false;
  CHECK(monitor.update(21000, metrics).heater_error);
  metrics.error_known = true;
  metrics.error_code = 0;
  CHECK(monitor.update(22000, metrics).active_count == 0);
}

static void offline_grace_recovery_and_wrap() {
  Monitor monitor;
  CHECK(monitor.begin_boot(3, 0, 2000));
  Metrics metrics;
  CHECK(!monitor.update(31999, metrics).offline);
  CHECK(monitor.update(32000, metrics).offline);
  metrics.online = true;
  CHECK(!monitor.update(33000, metrics).offline);
  metrics.online = false;
  metrics.ever_seen = true;
  metrics.last_valid_ms = 40000;
  CHECK(!monitor.update(69999, metrics).offline);
  CHECK(monitor.update(70000, metrics).offline);
  metrics.last_valid_ms = 70000;
  CHECK(!monitor.update(70001, metrics).offline);
  auto settings = monitor.settings();
  settings.offline_seconds = 10;
  CHECK(monitor.configure(settings));
  metrics.last_valid_ms = UINT32_MAX - 5000;
  CHECK(!monitor.update(4998, metrics).offline);
  CHECK(monitor.update(4999, metrics).offline);
  metrics.online = true;
  CHECK(!monitor.update(5000, metrics).offline);
  // Without an explicit boot event, update() establishes its own startup grace.
  Monitor standalone;
  Metrics unseen;
  CHECK(!standalone.update(100000, unseen).offline);
  CHECK(!standalone.update(129999, unseen).offline);
  CHECK(standalone.update(130000, unseen).offline);
}

int main() {
  settings_validation_and_transaction();
  favorite_utf8_and_lengths();
  boots_ring_and_snapshot_restore();
  clocks_and_corrupt_snapshots();
  alert_thresholds_recovery_and_masks();
  offline_grace_recovery_and_wrap();
  std::cout << "PASS: " << checks << " checks across 6 health scenarios\n";
}
