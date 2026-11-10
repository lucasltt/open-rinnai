// Host-only regression for history payload size/capacity; no heater or network.
#include "../esphome/dashboard_history_json.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; \
  std::exit(EXIT_FAILURE); } } while (false)

struct Fixture { const char *name; std::string json; size_t capacity, reserved; };
static std::vector<Fixture> fixtures;

static void reset() {
  esphome::test_millis = 1000;
  heater_stats::detail::accounting = heater_stats::detail::Accounting{};
  rinnai_storage::history = heater_history::History{};
}

static std::string capture(const char *name) {
  const auto reserved = 512U + heater_stats::point_count() * 48U + rinnai_storage::history.count() * 400U;
  std::string reservation_probe;
  reservation_probe.reserve(reserved);
  auto json = rinnai_dashboard::history_json(esphome::test_millis);
  CHECK(!json.empty() && json.front() == '{' && json.back() == '}');
  CHECK(json.size() <= reserved);
  CHECK(json.capacity() == reservation_probe.capacity());  // No append caused capacity-doubling growth.
  CHECK(json.find("\"session_capacity\":24") != std::string::npos);
  CHECK(json.find("\"window_s\":86400") != std::string::npos);
  CHECK(json.find("nan") == std::string::npos && json.find("inf") == std::string::npos);
  const auto capacity = json.capacity();
  fixtures.push_back({name, json, capacity, reserved});
  return json;
}

static void empty_and_legacy() {
  reset();
  const auto empty = capture("empty");
  CHECK(empty.find("\"samples\":[]") != std::string::npos);
  CHECK(empty.find("\"sessions\":[]") != std::string::npos);
  CHECK(fixtures.back().capacity < 1024);

  heater_history::LegacySnapshot legacy{};
  legacy.count = 2;
  for (size_t i = 0; i < legacy.count; ++i) {
    auto &record = legacy.records[i];
    record.sequence = 6 + static_cast<uint32_t>(i);
    record.duration_ms = 60000;
    record.heating_seconds = 55;
    record.water_l = 1.5 + i;
    record.energy_kwh = .125 + i;
    record.end_epoch_s = i ? 1577836860ULL : 0;
    record.complete = 1;
    record.end_reason = static_cast<uint8_t>(heater_stats::EndReason::FLOW_STOP);
  }
  CHECK(rinnai_storage::history.restore(legacy));
  const auto json = capture("legacy_two");
  CHECK(fixtures.back().capacity < 2048);
  CHECK(json.find("\"started_at_epoch_s\":null") != std::string::npos);
  CHECK(json.find("\"elapsed_s\":null") != std::string::npos);
  CHECK(json.find("\"age_s\":null") != std::string::npos);
  CHECK(json.find("\"cost\":null") != std::string::npos);
}

static void fill_extreme_sessions() {
  heater_history::Snapshot snapshot{};
  snapshot.count = 24;
  for (size_t i = 0; i < snapshot.count; ++i) {
    auto &record = snapshot.records[i];
    record.sequence = UINT32_MAX - 23U + static_cast<uint32_t>(i);
    record.duration_ms = UINT32_MAX;
    record.heating_seconds = UINT32_MAX / 1000U;
    // Values above physical full-register flow/power integrated over UINT32_MAX ms.
    record.water_l = 50000000.0 + i;
    record.energy_kwh = 600000.0 + i;
    record.start_epoch_s = 1577836800ULL;
    record.end_epoch_s = 4102444800ULL;
    record.complete = i % 2 ? 0 : 1;
    record.end_reason = static_cast<uint8_t>(record.complete ? heater_stats::EndReason::FLOW_STOP : heater_stats::EndReason::INTEGRATION_GAP);
  }
  CHECK(rinnai_storage::history.restore(snapshot));
  heater_stats::configure({heater_stats::GasType::GN, 100000, 100000});
}

static void extreme_sessions_and_full_plot() {
  reset();
  fill_extreme_sessions();
  const auto sessions = capture("extreme_24");
  CHECK(fixtures.back().capacity < 11000);
  CHECK(sessions.find("\"sequence\":4294967295") != std::string::npos);
  CHECK(sessions.find("\"started_at_epoch_s\":1577836800") != std::string::npos);
  CHECK(sessions.find("\"elapsed_s\":2524608000") != std::string::npos);
  CHECK(sessions.find("integration_gap") != std::string::npos);

  using heater_stats::Signal;
  heater_stats::observe(Signal::FLOW_L_MIN, 0, esphome::test_millis);
  for (size_t i = 0; i < 720; ++i) {
    esphome::test_millis += heater_stats::POINT_INTERVAL_MS;
    heater_stats::observe(Signal::INLET_C, 655.35, esphome::test_millis);
    heater_stats::observe(Signal::OUTLET_C, 655.35, esphome::test_millis);
    heater_stats::observe(Signal::FLOW_L_MIN, 655.35, esphome::test_millis);
    heater_stats::observe(Signal::POWER_KCAL_MIN, 6553.5, esphome::test_millis);
    heater_stats::observe(Signal::FLAME, 1, esphome::test_millis);
    heater_stats::observe(Signal::FLOW_L_MIN, 0, esphome::test_millis);
    heater_stats::tick();
  }
  CHECK(heater_stats::point_count() == 720);
  const auto full = capture("full_720_24");
  CHECK(full.size() < 48000 && fixtures.back().capacity < 48000);
  CHECK(full.find("655.35,655.35,655.35,457.30,1") != std::string::npos);
  const auto oldest = heater_stats::point(0).ms;
  const auto newest = heater_stats::point(719).ms;
  CHECK(newest - oldest == 719U * heater_stats::POINT_INTERVAL_MS);

  esphome::test_millis += heater_stats::POINT_INTERVAL_MS + 1U;
  // No observations beyond the bin boundary: values expire, without backfill.
  heater_stats::tick();
  CHECK(heater_stats::point_count() == 720);
  CHECK(heater_stats::point(0).ms == oldest + heater_stats::POINT_INTERVAL_MS);
  CHECK(heater_stats::point(719).ms == esphome::test_millis);
  const auto overflow = capture("overflow_720_24");
  CHECK(overflow.find("[0,null,null,null,null,null]") != std::string::npos);
  CHECK(fixtures.back().capacity < 48000);
}

int main() {
  empty_and_legacy();
  extreme_sessions_and_full_plot();
  std::cout << "{\"checks\":" << checks << ",\"fixtures\":[";
  for (size_t i = 0; i < fixtures.size(); ++i) {
    if (i) std::cout << ',';
    const auto &fixture = fixtures[i];
    std::cout << "{\"name\":\"" << fixture.name << "\",\"bytes\":" << fixture.json.size()
              << ",\"capacity\":" << fixture.capacity << ",\"reserved\":" << fixture.reserved
              << ",\"payload\":" << fixture.json << '}';
  }
  std::cout << "],\"passed\":true}\n";
}
