#pragma once

// Portable history JSON serialization, shared by the ESP backend and host tests.
#include <cmath>
#include <cstdio>
#include <string>
#include "heater_storage.h"

namespace rinnai_dashboard {

inline void append_number(std::string &out, double value, unsigned decimals = 3) {
  if (!std::isfinite(value)) { out += "null"; return; }
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.*f", static_cast<int>(decimals), value);
  out += buffer;
}

inline void append_uint(std::string &out, uint32_t value) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%u", static_cast<unsigned>(value));
  out += buffer;
}

inline double available_cost(const heater_stats::Estimate &estimate) {
  if (!estimate.gas_cost_valid && !estimate.water_cost_valid) return NAN;
  return (estimate.gas_cost_valid ? estimate.gas_cost : 0) +
         (estimate.water_cost_valid ? estimate.water_cost : 0);
}

inline std::string history_json(uint32_t now_ms) {
  // Reserve only for the rows currently present. A fixed 48KB reservation
  // wastes scarce ESP heap and makes cache replacement fail before filling.
  std::string out;
  out.reserve(512U + heater_stats::point_count() * 48U + rinnai_storage::history.count() * 400U);
  out = "{\"samples\":[";
  for (size_t i = 0; i < heater_stats::point_count(); ++i) {
    const auto point = heater_stats::point(i);
    if (i) out += ',';
    out += '['; append_uint(out, static_cast<uint32_t>(now_ms - point.ms) / 1000U);
    out += ','; append_number(out, point.inlet_c, 2);
    out += ','; append_number(out, point.outlet_c, 2);
    out += ','; append_number(out, point.flow_l_min, 2);
    out += ','; append_number(out, point.power_kcal_min * 0.06978, 2);
    out += ','; out += point.flame < 0 ? "null" : point.flame ? "1" : "0";
    out += ']';
  }
  out += "],\"sessions\":[";
  const auto epoch = rinnai_storage::epoch_now();
  for (size_t i = 0; i < rinnai_storage::history.count(); ++i) {
    const auto index = rinnai_storage::history.count() - 1 - i;
    const auto session = rinnai_storage::history.record(index);
    const auto cost = heater_stats::estimate(session.water_l, session.energy_kwh);
    if (i) out += ',';
    out += "{\"sequence\":"; append_uint(out, session.sequence);
    out += ",\"age_s\":"; append_number(out, rinnai_storage::history.age_s(index, now_ms, epoch), 1);
    out += ",\"ended_at_epoch_s\":";
    if (session.end_epoch_s) append_number(out, static_cast<double>(session.end_epoch_s), 0); else out += "null";
    out += ",\"started_at_epoch_s\":";
    if (session.start_epoch_s) append_number(out, static_cast<double>(session.start_epoch_s), 0); else out += "null";
    out += ",\"elapsed_s\":";
    if (session.start_epoch_s && session.end_epoch_s >= session.start_epoch_s)
      append_number(out, static_cast<double>(session.end_epoch_s - session.start_epoch_s), 0);
    else out += "null";
    out += ",\"duration_s\":"; append_number(out, session.duration_ms / 1000.0, 1);
    out += ",\"water_l\":"; append_number(out, session.water_l, 3);
    out += ",\"energy_kwh\":"; append_number(out, session.energy_kwh, 5);
    out += ",\"gas\":"; append_number(out, cost.gas_units, 5);
    out += ",\"cost\":"; append_number(out, available_cost(cost), 3);
    out += ",\"complete\":"; out += session.complete ? "true" : "false";
    out += ",\"end_reason\":\"";
    out += heater_stats::end_reason_name(static_cast<heater_stats::EndReason>(session.end_reason)); out += '"';
    out += '}';
  }
  out += "],\"ram_only\":false,\"points_ram_only\":true,\"sessions_persisted\":true,\"total_sessions\":";
  append_uint(out, heater_stats::totals().sessions);
  out += ",\"window_s\":86400,\"point_interval_s\":120,\"session_capacity\":24,";
  out += "\"sample_columns\":[\"age_s\",\"inlet_c\",\"outlet_c\",\"flow_l_min\",\"power_kw\",\"heating\"]}";
  return out;
}

}  // namespace rinnai_dashboard
