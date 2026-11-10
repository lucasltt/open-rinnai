#pragma once

// Compact, peripheral-free export of completed and durably committed uses.
// Identity never depends on wall-clock time or a currently configured tariff.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "heater_history.h"
#include "heater_stats.h"

namespace rinnai_sessions {

struct Snapshot {
  heater_history::Snapshot history{};
  std::array<uint8_t, 16> stream_id{};
  heater_stats::Settings settings{};
  uint32_t total_sessions{0};
  bool ready{false}, storage_ok{false};
};
static_assert(sizeof(Snapshot) <= 1224 && std::is_trivially_copyable<Snapshot>::value,
              "HTTP sessions snapshot must remain a compact POD");

inline bool valid_record(const heater_history::Record &row) {
  constexpr uint64_t max_epoch = 253402300799ULL;  // End of year 9999, representable in HA.
  const bool start_valid = row.start_epoch_s == 0 ||
      (row.start_epoch_s >= heater_history::MIN_VALID_EPOCH_S && row.start_epoch_s <= max_epoch);
  const bool end_valid = row.end_epoch_s == 0 ||
      (row.end_epoch_s >= heater_history::MIN_VALID_EPOCH_S && row.end_epoch_s <= max_epoch);
  return row.sequence != 0 && std::isfinite(row.water_l) && row.water_l >= 0 &&
      std::isfinite(row.energy_kwh) && row.energy_kwh >= 0 && start_valid && end_valid &&
      (!row.start_epoch_s || !row.end_epoch_s || row.start_epoch_s <= row.end_epoch_s) &&
      row.heating_seconds <= row.duration_ms / 1000U && row.complete <= 1 &&
      row.end_reason >= static_cast<uint8_t>(heater_stats::EndReason::FLOW_STOP) &&
      row.end_reason <= static_cast<uint8_t>(heater_stats::EndReason::INTEGRATION_GAP) &&
      (row.complete != 0) == (row.end_reason == static_cast<uint8_t>(heater_stats::EndReason::FLOW_STOP)) &&
      row.reserved[0] == 0 && row.reserved[1] == 0;
}

inline bool valid(const Snapshot &snapshot) {
  if (!snapshot.ready || !snapshot.storage_ok || snapshot.history.version != heater_history::SNAPSHOT_VERSION ||
      snapshot.history.reserved != 0 || snapshot.history.count > heater_history::CAPACITY ||
      (snapshot.settings.gas_type != heater_stats::GasType::GN && snapshot.settings.gas_type != heater_stats::GasType::GLP) ||
      !std::isfinite(snapshot.settings.gas_tariff_per_unit) || snapshot.settings.gas_tariff_per_unit < 0 ||
      !std::isfinite(snapshot.settings.water_tariff_per_m3) || snapshot.settings.water_tariff_per_m3 < 0) return false;
  bool identified = false;
  for (const auto value : snapshot.stream_id) identified = identified || value != 0;
  if (!identified) return false;
  uint32_t previous = 0;
  for (size_t i = 0; i < snapshot.history.count; ++i) {
    const auto &row = snapshot.history.records[i];
    if (!valid_record(row) || row.sequence <= previous || row.sequence > snapshot.total_sessions) return false;
    previous = row.sequence;
  }
  const heater_history::Record empty{};
  for (size_t i = snapshot.history.count; i < heater_history::CAPACITY; ++i)
    if (std::memcmp(&snapshot.history.records[i], &empty, sizeof(empty)) != 0) return false;
  return true;
}

inline void append_uint(std::string &out, uint64_t value) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  out += buffer;
}

inline void append_number(std::string &out, double value) {
  if (!std::isfinite(value)) { out += "null"; return; }
  char buffer[48];
  // General notation avoids truncating extreme finite values in a fixed buffer.
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  out += buffer;
}

inline void append_epoch(std::string &out, uint64_t value) {
  if (value) append_uint(out, value); else out += "null";
}

inline bool safe_version(const char *version) {
  if (!version) return false;
  const size_t size = std::strlen(version);
  if (size == 0 || size > 32) return false;
  for (size_t i = 0; i < size; ++i) {
    const char c = version[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          c == '.' || c == '-' || c == '_')) return false;
  }
  return true;
}

inline std::string serialize(const Snapshot &snapshot, const char *device_id, const char *firmware_version) {
  if (!valid(snapshot) || !device_id || std::strlen(device_id) != 19 ||
      std::strncmp(device_id, "rinnai-", 7) != 0 || !safe_version(firmware_version)) return {};
  for (size_t i = 7; i < 19; ++i)
    if (!((device_id[i] >= '0' && device_id[i] <= '9') || (device_id[i] >= 'a' && device_id[i] <= 'f'))) return {};
  char stream[33];
  static constexpr char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < snapshot.stream_id.size(); ++i) {
    stream[2 * i] = hex[snapshot.stream_id[i] >> 4U];
    stream[2 * i + 1] = hex[snapshot.stream_id[i] & 15U];
  }
  stream[32] = '\0';
  const bool glp = snapshot.settings.gas_type == heater_stats::GasType::GLP;
  const char *unit = glp ? "kg" : "m3";
  const double gas_factor = 3600.0 / (heater_stats::KJ_PER_KCAL *
      (glp ? heater_stats::GLP_KCAL_PER_KG : heater_stats::GN_KCAL_PER_M3));
  const auto capacity = 1024U + snapshot.history.count * 640U;
  std::string out;
  out.reserve(capacity);
  out = "{\"api_version\":\"1\",\"schema_version\":1,\"device_id\":\""; out += device_id;
  out += "\",\"stream_id\":\""; out += stream;
  out += "\",\"firmware_version\":\""; out += firmware_version;
  out += "\",\"ready\":true,\"storage_ok\":true,\"capacity\":24,\"total_sessions\":";
  append_uint(out, snapshot.total_sessions);
  out += ",\"oldest_sequence\":";
  if (snapshot.history.count) append_uint(out, snapshot.history.records[0].sequence); else out += "null";
  out += ",\"newest_sequence\":";
  if (snapshot.history.count) append_uint(out, snapshot.history.records[snapshot.history.count - 1].sequence); else out += "null";
  out += ",\"sequence_exhausted\":"; out += snapshot.total_sessions == UINT32_MAX ? "true" : "false";
  out += ",\"pricing_basis\":\"current_settings\",\"settings\":{\"gas_type\":\""; out += glp ? "GLP" : "GN";
  out += "\",\"gas_unit\":\""; out += unit; out += "\",\"gas_tariff\":";
  append_number(out, snapshot.settings.gas_tariff_per_unit);
  out += ",\"water_tariff\":"; append_number(out, snapshot.settings.water_tariff_per_m3);
  out += "},\"sessions\":[";
  for (size_t i = 0; i < snapshot.history.count; ++i) {
    const auto &row = snapshot.history.records[i];
    const double gas = row.energy_kwh * gas_factor;
    const double gas_cost = snapshot.settings.gas_tariff_per_unit > 0 ? gas * snapshot.settings.gas_tariff_per_unit : NAN;
    const double water_cost = snapshot.settings.water_tariff_per_m3 > 0 ? row.water_l / 1000.0 * snapshot.settings.water_tariff_per_m3 : NAN;
    const bool gas_valid = std::isfinite(gas_cost), water_valid = std::isfinite(water_cost);
    const double cost = gas_valid || water_valid ? (gas_valid ? gas_cost : 0) + (water_valid ? water_cost : 0) : NAN;
    if (i) out += ',';
    out += "{\"id\":\""; out += stream; out += ':'; append_uint(out, row.sequence);
    out += "\",\"sequence\":"; append_uint(out, row.sequence);
    out += ",\"started_at_epoch_s\":"; append_epoch(out, row.start_epoch_s);
    out += ",\"ended_at_epoch_s\":"; append_epoch(out, row.end_epoch_s);
    out += ",\"elapsed_s\":";
    if (row.start_epoch_s && row.end_epoch_s) append_uint(out, row.end_epoch_s - row.start_epoch_s); else out += "null";
    out += ",\"duration_s\":"; append_number(out, row.duration_ms / 1000.0);
    out += ",\"heating_s\":"; append_uint(out, row.heating_seconds);
    out += ",\"water_l\":"; append_number(out, row.water_l);
    out += ",\"energy_kwh\":"; append_number(out, row.energy_kwh);
    out += ",\"gas\":"; append_number(out, gas);
    out += ",\"gas_unit\":\""; out += unit; out += '"';
    out += ",\"gas_cost\":"; append_number(out, gas_cost);
    out += ",\"water_cost\":"; append_number(out, water_cost);
    out += ",\"cost\":"; append_number(out, cost);
    out += ",\"cost_complete\":"; out += gas_valid && water_valid && std::isfinite(cost) ? "true" : "false";
    out += ",\"complete\":"; out += row.complete ? "true" : "false";
    out += ",\"end_reason\":\"";
    out += heater_stats::end_reason_name(static_cast<heater_stats::EndReason>(row.end_reason));
    out += "\"}";
  }
  out += "]}";
  if (out.size() > capacity) return {};  // Bound is exercised by host fixtures.
  return out;
}

}  // namespace rinnai_sessions
