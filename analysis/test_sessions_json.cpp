#include <ArduinoJson.h>
#include "../esphome/heater_sessions.h"
#include <cstdlib>
#include <iostream>
#include <limits>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; std::exit(EXIT_FAILURE); } } while (false)

static constexpr const char *DEVICE = "rinnai-020000000001";
static constexpr uint64_t EPOCH = 1793000000ULL;
static rinnai_sessions::Snapshot snapshot() {
  rinnai_sessions::Snapshot value;
  value.ready = value.storage_ok = true;
  value.stream_id[0] = 1;
  return value;
}
static heater_history::Record record(uint32_t sequence = 1) {
  heater_history::Record row;
  row.sequence = sequence;
  row.start_epoch_s = EPOCH;
  row.end_epoch_s = EPOCH + 65;
  row.duration_ms = 60000;
  row.heating_seconds = 55;
  row.water_l = 10;
  row.energy_kwh = .125;
  row.complete = 1;
  row.end_reason = static_cast<uint8_t>(heater_stats::EndReason::FLOW_STOP);
  return row;
}
static JsonDocument parse(const rinnai_sessions::Snapshot &value) {
  const auto text = rinnai_sessions::serialize(value, DEVICE, "1.2.0");
  CHECK(!text.empty());
  CHECK(text.size() <= 1024U + value.history.count * 640U);
  JsonDocument document;
  CHECK(!deserializeJson(document, text));
  return document;
}

static void empty_and_unknown() {
  auto value = snapshot();
  auto doc = parse(value);
  CHECK(doc["api_version"].as<std::string>() == "1");
  CHECK(doc["schema_version"].as<int>() == 1 && doc["capacity"].as<int>() == 24);
  CHECK(doc["oldest_sequence"].isNull() && doc["newest_sequence"].isNull());
  CHECK(doc["sessions"].size() == 0 && !doc["sequence_exhausted"].as<bool>());
  CHECK(doc["stream_id"].as<std::string>() == "01000000000000000000000000000000");
  value.history.count = 1; value.total_sessions = 1; value.history.records[0] = record();
  value.history.records[0].start_epoch_s = 0;
  doc = parse(value);
  CHECK(doc["sessions"][0]["started_at_epoch_s"].isNull());
  CHECK(doc["sessions"][0]["elapsed_s"].isNull());
  CHECK(doc["sessions"][0]["duration_s"].as<double>() == 60);
  CHECK(doc["sessions"][0]["heating_s"].as<unsigned>() == 55);
  const auto id = doc["sessions"][0]["id"].as<std::string>();
  value.history.records[0].start_epoch_s = EPOCH;
  doc = parse(value);
  CHECK(doc["sessions"][0]["id"].as<std::string>() == id);
  CHECK(doc["sessions"][0]["elapsed_s"].as<unsigned>() == 65);
  value.history.records[0].end_epoch_s = 0;
  doc = parse(value);
  CHECK(doc["sessions"][0]["ended_at_epoch_s"].isNull() && doc["sessions"][0]["elapsed_s"].isNull());
}

static void prices_and_partial() {
  auto value = snapshot(); value.history.count = 1; value.total_sessions = 1; value.history.records[0] = record();
  auto doc = parse(value);
  CHECK(doc["sessions"][0]["gas_cost"].isNull() && doc["sessions"][0]["water_cost"].isNull());
  CHECK(doc["sessions"][0]["cost"].isNull() && !doc["sessions"][0]["cost_complete"].as<bool>());
  const auto id = doc["sessions"][0]["id"].as<std::string>();
  value.settings.gas_tariff_per_unit = 8.5;
  doc = parse(value);
  const auto gas_cost = doc["sessions"][0]["gas_cost"].as<double>();
  CHECK(gas_cost > 0 && doc["sessions"][0]["water_cost"].isNull());
  CHECK(doc["sessions"][0]["cost"].as<double>() == gas_cost);
  CHECK(!doc["sessions"][0]["cost_complete"].as<bool>());
  value.settings.water_tariff_per_m3 = 12;
  doc = parse(value);
  CHECK(std::abs(doc["sessions"][0]["water_cost"].as<double>() - .12) < 1e-6);
  CHECK(std::abs(doc["sessions"][0]["cost"].as<double>() - gas_cost - .12) < 1e-6);
  CHECK(doc["sessions"][0]["cost_complete"].as<bool>());
  CHECK(doc["sessions"][0]["id"].as<std::string>() == id);
  value.settings.gas_tariff_per_unit = 0;
  doc = parse(value);
  CHECK(doc["sessions"][0]["gas_cost"].isNull());
  CHECK(std::abs(doc["sessions"][0]["cost"].as<double>() - .12) < 1e-6);
  value.settings.gas_type = heater_stats::GasType::GLP;
  doc = parse(value);
  CHECK(doc["settings"]["gas_unit"].as<std::string>() == "kg");
  CHECK(doc["sessions"][0]["gas_unit"].as<std::string>() == "kg");
  CHECK(doc["pricing_basis"].as<std::string>() == "current_settings");
  value.history.records[0].complete = 0;
  value.history.records[0].end_reason = static_cast<uint8_t>(heater_stats::EndReason::INTEGRATION_GAP);
  doc = parse(value);
  CHECK(!doc["sessions"][0]["complete"].as<bool>());
  CHECK(doc["sessions"][0]["end_reason"].as<std::string>() == "integration_gap");
}

static void full_and_extreme() {
  auto value = snapshot(); value.history.count = 24; value.total_sessions = UINT32_MAX;
  value.settings = {heater_stats::GasType::GN, 100000, 100000};
  for (size_t i = 0; i < 24; ++i) {
    auto &row = value.history.records[i]; row = record(UINT32_MAX - 23U + static_cast<uint32_t>(i));
    row.water_l = row.energy_kwh = std::numeric_limits<double>::max() / 16.0;
    row.duration_ms = UINT32_MAX; row.heating_seconds = UINT32_MAX / 1000U;
    row.start_epoch_s = heater_history::MIN_VALID_EPOCH_S; row.end_epoch_s = 253402300799ULL;
  }
  auto doc = parse(value);
  CHECK(doc["sessions"].size() == 24 && doc["sequence_exhausted"].as<bool>());
  CHECK(doc["oldest_sequence"].as<uint32_t>() == UINT32_MAX - 23U);
  CHECK(doc["newest_sequence"].as<uint32_t>() == UINT32_MAX);
  for (size_t i = 0; i < 24; ++i) {
    CHECK(doc["sessions"][i]["sequence"].as<uint32_t>() == UINT32_MAX - 23U + static_cast<uint32_t>(i));
    CHECK(std::abs(doc["sessions"][i]["water_l"].as<double>() / (std::numeric_limits<double>::max() / 16.0) - 1.0) < 1e-6);
    CHECK(doc["sessions"][i]["gas_cost"].isNull() && doc["sessions"][i]["water_cost"].isNull());
  }
  const auto text = rinnai_sessions::serialize(value, DEVICE, "1.2.0");
  std::cout << "Full 24-use JSON: " << text.size() << " bytes, " << text.capacity() << " capacity\n";
  CHECK(text.size() <= 16384 && text.capacity() <= 16448);
}

static void invalid_sources_are_not_serialized() {
  auto value = snapshot(); value.history.count = 1; value.total_sessions = 1; value.history.records[0] = record();
  auto reject = [&](rinnai_sessions::Snapshot bad) {
    CHECK(rinnai_sessions::serialize(bad, DEVICE, "1.2.0").empty());
  };
  auto bad = value; bad.ready = false; reject(bad);
  bad = value; bad.storage_ok = false; reject(bad);
  bad = value; bad.stream_id = {}; reject(bad);
  bad = value; bad.history.count = 25; reject(bad);
  bad = value; bad.history.version = 3; reject(bad);
  bad = value; bad.history.records[0].water_l = NAN; reject(bad);
  bad = value; bad.history.records[0].energy_kwh = -1; reject(bad);
  bad = value; bad.history.records[0].heating_seconds = 61; reject(bad);
  bad = value; bad.history.records[0].sequence = 0; reject(bad);
  bad = value; bad.total_sessions = 0; reject(bad);
  bad = value; bad.history.records[0].start_epoch_s = EPOCH + 66; reject(bad);
  bad = value; bad.history.records[0].end_epoch_s = UINT64_MAX; reject(bad);
  bad = value; bad.history.records[0].complete = 0; reject(bad);
  bad = value; bad.history.records[0].reserved[0] = 1; reject(bad);
  bad = value; bad.history.records[1] = record(2); reject(bad);  // Nonzero unused slot.
  bad = value; bad.history.count = 2; bad.total_sessions = 2; bad.history.records[1] = record(1); reject(bad);
  bad = value; bad.settings.gas_tariff_per_unit = INFINITY; reject(bad);
  CHECK(rinnai_sessions::serialize(value, "rinnai-02000000000A", "1.2.0").empty());
  CHECK(rinnai_sessions::serialize(value, DEVICE, "1.2.0\"}").empty());
  CHECK(rinnai_sessions::serialize(value, nullptr, "1.2.0").empty());
}

int main() {
  empty_and_unknown(); prices_and_partial(); full_and_extreme(); invalid_sources_are_not_serialized();
  std::cout << "Sessions JSON: " << checks << " checks passed\n";
}
