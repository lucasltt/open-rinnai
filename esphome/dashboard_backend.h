#pragma once

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <cctype>
#include <esp_heap_caps.h>
#include "esphome/components/json/json_util.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "heater_runtime.h"
#include "heater_stats.h"
#include "heater_storage.h"
#include "heater_sessions.h"
#include "heater_diagnostics.h"
#include "dashboard_history_json.h"
#include "favicon_asset.h"
#include "pwa_assets.h"
#include "api_assets.h"
#include "api_requests.h"
#include "api_command.h"

namespace rinnai_dashboard {

struct Live {
  const char *version{"1.2.0"};
  bool online{false}, heating{false}, controls_ready{false};
  bool storage_ok{true}, archive_loaded{false};
  double inlet_c{NAN}, outlet_c{NAN}, flow_l_min{NAN}, power_kcal_min{NAN};
  double rssi_dbm{NAN}, heap_bytes{NAN};
  uint32_t uptime_ms{0}, latency_ms{0}, valid_frames{0}, timeouts{0}, bad_frames{0}, last_seen_ms{0};
  int error_code{-1};
};

struct SettingsRequest {
  bool glp{false};
  double gas_tariff{0}, water_tariff{0};
};

template<typename Target> inline void number(Target target, double value) {
  if (std::isfinite(value)) target.set(value); else target.set(nullptr);
}

inline void add_accounting(JsonObject target, double water_l, double energy_kwh) {
  const auto estimate = heater_stats::estimate(water_l, energy_kwh);
  number(target["water_l"], water_l);
  number(target["energy_kwh"], energy_kwh);
  number(target["gas"], estimate.gas_units);
  number(target["cost"], available_cost(estimate));
  target["cost_complete"] = estimate.cost_valid;
  number(target["gas_cost"], estimate.gas_cost_valid ? estimate.gas_cost : NAN);
  number(target["water_cost"], estimate.water_cost_valid ? estimate.water_cost : NAN);
}

inline void add_bucket(JsonObject target, const heater_periods::Bucket &bucket) {
  target["key"] = bucket.key;
  add_accounting(target, bucket.water_l, bucket.energy_kwh);
  target["heating_s"] = bucket.heating_seconds;
  target["sessions"] = bucket.sessions;
  target["partial"] = bucket.partial != 0;
}

inline void add_alert(JsonArray array, const char *id, const char *title, const std::string &message,
                      const char *severity) {
  auto item = array.add<JsonObject>();
  item["id"] = id; item["title"] = title; item["message"] = message; item["severity"] = severity;
}

// The SDK builder doubles its serialization buffer and then copies it into a
// std::string. Large snapshots briefly needed both buffers and could abort under
// full-history HTTP traffic. Measure once and serialize into the final string.
inline std::string serialize_snapshot(JsonDocument &document) {
  if (document.overflowed()) return {};  // Preserve the previous valid cache.
  const auto size = measureJson(document);
  std::string result;
  result.reserve(size);
  if (serializeJson(document, result) != size) return {};
  return result;
}

template<typename Callback> inline std::string build_snapshot(Callback callback) {
  JsonDocument document;
  callback(document.to<JsonObject>());
  return serialize_snapshot(document);
}

inline std::string state_json(const Live &live) {
  return build_snapshot([&](JsonObject root) {
    root["version"] = live.version;
    root["online"] = live.online;
    root["heating"] = live.heating && live.online;
    number(root["inlet_c"], live.inlet_c);
    number(root["outlet_c"], live.outlet_c);
    number(root["flow_l_min"], live.flow_l_min);
    number(root["power_kcal_min"], live.power_kcal_min);
    number(root["power_kw"], live.power_kcal_min * 0.06978);
    const auto &controller = rinnai_smart::controller;
    const double selected = controller.actual_celsius() >= 35 ? controller.actual_celsius() : NAN;
    number(root["setpoint_c"], selected);
    number(root["target_c"], controller.desired() >= 35 ? controller.desired() : selected);
    auto control = root["control"].to<JsonObject>();
    control["busy"] = controller.busy();
    control["status"] = controller.status();
    control["ready"] = live.controls_ready;
    control["uncertain"] = controller.uncertain();
    control["confirmed_steps"] = controller.confirmed_steps();
    auto session = root["session"].to<JsonObject>();
    const auto current = heater_stats::current_session();
    add_accounting(session, current.water_l, current.energy_kwh);
    session["active"] = current.active;
    session["complete"] = current.complete;
    session["duration_s"] = current.duration_ms / 1000.0;
    session["heating_s"] = current.heating_seconds;
    session["end_reason"] = heater_stats::end_reason_name(current.end_reason);
    const auto session_epoch = rinnai_storage::epoch_now();
    const bool has_session = current.sequence != 0;
    const auto since_start_ms = static_cast<uint32_t>(live.uptime_ms - current.start_ms);
    const auto elapsed_ms = current.active ? since_start_ms : static_cast<uint32_t>(current.end_ms - current.start_ms);
    session["sequence"] = current.sequence;
    number(session["elapsed_s"], has_session ? elapsed_ms / 1000.0 : NAN);
    if (has_session && session_epoch && session_epoch >= since_start_ms / 1000U)
      session["started_at_epoch_s"] = session_epoch - since_start_ms / 1000U;
    else session["started_at_epoch_s"] = nullptr;
    auto totals_json = root["totals"].to<JsonObject>();
    const auto totals = heater_stats::totals();
    add_accounting(totals_json, totals.water_l, totals.energy_kwh);
    totals_json["heating_s"] = totals.heating_seconds;
    totals_json["sessions"] = totals.sessions;
    auto settings_json = root["settings"].to<JsonObject>();
    const auto settings = heater_stats::settings();
    settings_json["gas_type"] = settings.gas_type == heater_stats::GasType::GLP ? "GLP" : "GN";
    settings_json["gas_tariff"] = settings.gas_tariff_per_unit;
    settings_json["water_tariff"] = settings.water_tariff_per_m3;
    settings_json["gas_unit"] = settings.gas_type == heater_stats::GasType::GLP ? "kg" : "m3";
    const auto preferences = rinnai_storage::health.settings();
    auto favorites = settings_json["favorites"].to<JsonArray>();
    for (const auto &favorite : preferences.favorites) {
      auto item = favorites.add<JsonObject>();
      // This Settings copy dies when the JSON callback returns. std::string
      // makes ArduinoJson own the name through the later serialization step.
      item["name"] = std::string(favorite.name); item["temp_c"] = favorite.temperature_c;
    }
    auto alert_settings = settings_json["alerts"].to<JsonObject>();
    alert_settings["long_use_enabled"] = (preferences.enabled_mask & heater_health::LONG_USE) != 0;
    alert_settings["flow_enabled"] = (preferences.enabled_mask & heater_health::CONTINUOUS_FLOW) != 0;
    alert_settings["error_enabled"] = (preferences.enabled_mask & heater_health::HEATER_ERROR) != 0;
    alert_settings["offline_enabled"] = (preferences.enabled_mask & heater_health::OFFLINE) != 0;
    alert_settings["long_use_min"] = preferences.long_use_min;
    alert_settings["continuous_flow_min"] = preferences.continuous_flow_min;
    alert_settings["offline_seconds"] = preferences.offline_seconds;
    const auto epoch = rinnai_storage::epoch_now();
    auto periods = root["periods"].to<JsonObject>();
    periods["clock_valid"] = heater_periods::clock_valid(epoch);
    const auto since = rinnai_storage::periods.tracked_since_epoch();
    if (since) periods["tracked_since_epoch_s"] = since; else periods["tracked_since_epoch_s"] = nullptr;
    add_bucket(periods["today"].to<JsonObject>(), rinnai_storage::periods.today(epoch));
    add_bucket(periods["month"].to<JsonObject>(), rinnai_storage::periods.month(epoch));
    const auto unknown = rinnai_storage::periods.unassigned();
    auto unassigned = periods["unassigned"].to<JsonObject>();
    add_accounting(unassigned, unknown.water_l, unknown.energy_kwh);
    unassigned["heating_s"] = unknown.heating_seconds; unassigned["sessions"] = unknown.sessions;
    auto readings = root["readings"].to<JsonObject>();
    auto add_reading = [&](const char *name, uint16_t address, uint32_t freshness_ms) {
      auto item = readings[name].to<JsonObject>();
      const auto seen = rinnai_telemetry::last_seen(address);
      number(item["age_s"], seen ? static_cast<uint32_t>(live.uptime_ms - seen) / 1000.0 : NAN);
      item["stale"] = !seen || static_cast<uint32_t>(live.uptime_ms - seen) >= freshness_ms;
    };
    add_reading("inlet", 0x0606, 30000); add_reading("outlet", 0x0604, 30000);
    add_reading("flow", 0x0608, 30000); add_reading("power", 0x0648, 30000);
    add_reading("setpoint", 0x060E, 15000); add_reading("flame", 0x060F, 30000);
    add_reading("error", 0x0614, 15000);
    auto alerts = root["alerts"].to<JsonArray>();
    const auto active = rinnai_storage::health.alerts();
    if (active.long_use) add_alert(alerts, "long_use", "Uso prolongado",
        "Agua quente em uso por mais de " + std::to_string(preferences.long_use_min) + " minutos.", "warning");
    if (active.continuous_flow) add_alert(alerts, "continuous_flow", "Agua circulando ha muito tempo",
        "Vazao continua por mais de " + std::to_string(preferences.continuous_flow_min) + " minutos.", "warning");
    if (active.heater_error) add_alert(alerts, "heater_error", "Atencao no aquecedor",
        active.heater_error_stale ? "O ultimo erro ainda aguarda confirmacao de normalizacao." :
        "O aquecedor informou o codigo " + std::to_string(live.error_code) + ".", "error");
    if (active.offline) add_alert(alerts, "offline", "Sem comunicacao com o aquecedor",
        "Nao chegaram respostas validas dentro do limite configurado.", "error");
    auto diag = root["diag"].to<JsonObject>();
    number(diag["rssi_dbm"], live.rssi_dbm);
    number(diag["heap_bytes"], live.heap_bytes);
    diag["uptime_s"] = live.uptime_ms / 1000.0;
    diag["latency_ms"] = live.latency_ms;
    diag["valid_frames"] = live.valid_frames;
    diag["timeouts"] = live.timeouts;
    diag["bad_frames"] = live.bad_frames;
    if (live.error_code >= 0) diag["error_code"] = live.error_code; else diag["error_code"] = nullptr;
    diag["last_seen_ms"] = live.last_seen_ms;
    number(diag["setpoint_age_s"], controller.actual_celsius() >= 35 ? controller.index_age_ms(live.uptime_ms) / 1000.0 : NAN);
    if (live.last_seen_ms) diag["last_seen_age_s"] = static_cast<uint32_t>(live.uptime_ms - live.last_seen_ms) / 1000.0;
    else diag["last_seen_age_s"] = nullptr;
    diag["tx_gpio"] = 25;
    diag["rx_gpio"] = 26;
    diag["history_persists"] = false;
    diag["session_history_persists"] = true;
    diag["session_history_restored"] = live.archive_loaded;
    diag["storage_ok"] = live.storage_ok;
    diag["clock_synchronized"] = rinnai_storage::epoch_now() != 0;
    diag["boot_count"] = rinnai_storage::health.boot_count();
    diag["crash_count"] = rinnai_storage::health.crash_count();
  });
}

inline void append_bucket(std::string &out, const heater_periods::Bucket &bucket) {
  const auto estimate = heater_stats::estimate(bucket.water_l, bucket.energy_kwh);
  out += "{\"key\":"; append_uint(out, bucket.key);
  out += ",\"water_l\":"; append_number(out, bucket.water_l);
  out += ",\"energy_kwh\":"; append_number(out, bucket.energy_kwh, 5);
  out += ",\"heating_s\":"; append_uint(out, bucket.heating_seconds);
  out += ",\"sessions\":"; append_uint(out, bucket.sessions);
  out += ",\"gas\":"; append_number(out, estimate.gas_units, 5);
  out += ",\"cost\":"; append_number(out, available_cost(estimate));
  out += ",\"cost_complete\":"; out += estimate.cost_valid ? "true" : "false";
  out += ",\"gas_cost\":"; append_number(out, estimate.gas_cost_valid ? estimate.gas_cost : NAN);
  out += ",\"water_cost\":"; append_number(out, estimate.water_cost_valid ? estimate.water_cost : NAN);
  out += ",\"partial\":"; out += bucket.partial ? "true" : "false";
  out += '}';
}

inline std::string periods_json() {
  std::string out;
  out.reserve(512 + 300 * (rinnai_storage::periods.day_count() + rinnai_storage::periods.month_count()));
  const auto epoch = rinnai_storage::epoch_now();
  out = "{\"clock_valid\":"; out += heater_periods::clock_valid(epoch) ? "true" : "false";
  out += ",\"tracked_since_epoch_s\":";
  const auto since = rinnai_storage::periods.tracked_since_epoch();
  if (since) append_number(out, static_cast<double>(since), 0); else out += "null";
  out += ",\"days\":[";
  for (size_t i = 0; i < rinnai_storage::periods.day_count(); ++i) {
    if (i) out += ',';
    append_bucket(out, rinnai_storage::periods.day(i));
  }
  out += "],\"months\":[";
  for (size_t i = 0; i < rinnai_storage::periods.month_count(); ++i) {
    if (i) out += ',';
    append_bucket(out, rinnai_storage::periods.month_record(i));
  }
  const auto unknown = rinnai_storage::periods.unassigned();
  out += "],\"unassigned\":{\"water_l\":"; append_number(out, unknown.water_l);
  out += ",\"energy_kwh\":"; append_number(out, unknown.energy_kwh, 5);
  out += ",\"heating_s\":"; append_uint(out, unknown.heating_seconds);
  out += ",\"sessions\":"; append_uint(out, unknown.sessions);
  out += "}}";
  return out;
}

inline std::string diagnostics_json() {
  return build_snapshot([&](JsonObject root) {
    const auto &health = rinnai_storage::health;
    root["boot_count"] = health.boot_count(); root["crash_count"] = health.crash_count();
    const auto last = health.boot_size() ? health.boot(health.boot_size() - 1) : heater_health::BootRecord{};
    root["last_reset_reason"] = rinnai_diagnostics::reset_reason_text(last.reason_code);
    root["clock_valid"] = rinnai_storage::epoch_now() != 0;
    root["storage_ok"] = rinnai_storage::storage_ok && rinnai_storage::health_storage_ok &&
        rinnai_storage::preferences_storage_ok && rinnai_diagnostics::storage_ok;
    root["firmware_elf_sha256"] = rinnai_diagnostics::current_identity.elf_sha256;
    root["heap_free_bytes"] = esp_get_free_heap_size();
    root["heap_largest_block_bytes"] = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    auto boots = root["boots"].to<JsonArray>();
    for (size_t i = 0; i < health.boot_size(); ++i) {
      const auto record = health.boot(health.boot_size() - 1 - i);
      auto item = boots.add<JsonObject>();
      item["sequence"] = record.sequence;
      if (record.epoch_s) item["epoch_s"] = record.epoch_s; else item["epoch_s"] = nullptr;
      item["reason"] = rinnai_diagnostics::reset_reason_text(record.reason_code);
      item["reason_code"] = record.reason_code;
      item["crash"] = heater_health::is_crash_reason(record.reason_code);
    }
    root["crash_log"] = rinnai_diagnostics::crash.text;
    root["crash_firmware_elf_sha256"] = rinnai_diagnostics::crash.elf_sha256;
    root["crash_firmware_version"] = rinnai_diagnostics::crash.project_version;
    root["crash_log_truncated"] = rinnai_diagnostics::crash.length >= sizeof(rinnai_diagnostics::crash.text) - 2;
    root["crash_record_boot"] = rinnai_diagnostics::crash.boot_sequence;
  });
}

class Backend : public AsyncWebHandler {
 public:
  bool begin() {
    if (registered_) return true;
    auto *base = esphome::web_server_base::global_web_server_base;
    if (!base) return false;
    base->add_handler(this);
    uint8_t mac[6]{};
    esphome::get_mac_address_raw(mac);
    std::snprintf(session_device_id_, sizeof(session_device_id_), "rinnai-%02x%02x%02x%02x%02x%02x",
        static_cast<unsigned>(mac[0]), static_cast<unsigned>(mac[1]), static_cast<unsigned>(mac[2]),
        static_cast<unsigned>(mac[3]), static_cast<unsigned>(mac[4]), static_cast<unsigned>(mac[5]));
    registered_ = true;
    return true;
  }
  bool canHandle(AsyncWebServerRequest *request) const override {
    const auto path = request->url();
    return path == "/api" || path.rfind("/api/", 0) == 0 ||
           rinnai_pwa::find(path.c_str()) || path == "/favicon.svg" || path == "/favicon.ico" ||
           path == "/rinnai/api/state" || path == "/rinnai/api/history" ||
           path == "/rinnai/api/periods" || path == "/rinnai/api/diagnostics" ||
           path == "/rinnai/api/preferences" || path == "/rinnai/api/temperature" || path == "/rinnai/api/settings";
  }
  void handleRequest(AsyncWebServerRequest *request) override {
    const auto path = request->url();
    if (const auto *asset = rinnai_api_assets::find(path.c_str())) {
      if (request->method() != HTTP_GET) { api_error_(request, 405, "method_not_allowed", "Use GET para esta pagina."); return; }
      auto *response = request->beginResponse(200, asset->type, asset->data, asset->size);
      if (asset->compressed) response->addHeader("Content-Encoding", "gzip");
      response->addHeader("Cache-Control", "no-cache");
      response->addHeader("X-Content-Type-Options", "nosniff");
      request->send(response); return;
    }
    if (path == "/api" || path.rfind("/api/", 0) == 0) { handle_v1_(request, path); return; }
    if (const auto *asset = rinnai_pwa::find(path.c_str())) {
      if (request->method() != HTTP_GET) { respond_(request, 405, "{\"error\":\"Metodo nao permitido\"}"); return; }
      auto *response = request->beginResponse(200, asset->type, asset->data, asset->size);
      if (asset->compressed) response->addHeader("Content-Encoding", "gzip");
      response->addHeader("Cache-Control", "no-cache");
      if (path == "/sw.js") response->addHeader("Service-Worker-Allowed", "/");
      response->addHeader("X-Content-Type-Options", "nosniff");
      request->send(response);
      return;
    }
    if (request->method() == HTTP_GET && (path == "/favicon.svg" || path == "/favicon.ico")) {
      const bool svg = path == "/favicon.svg";
      const auto *data = svg ? reinterpret_cast<const uint8_t *>(rinnai_favicon::SVG) : rinnai_favicon::ICON;
      const auto length = svg ? sizeof(rinnai_favicon::SVG) - 1 : sizeof(rinnai_favicon::ICON);
      auto *response = request->beginResponse(200, svg ? "image/svg+xml" : "image/x-icon", data, length);
      response->addHeader("Cache-Control", "public, max-age=86400");
      request->send(response);
      return;
    }
    if (request->method() == HTTP_GET && (path == "/rinnai/api/state" || path == "/rinnai/api/history" ||
        path == "/rinnai/api/periods" || path == "/rinnai/api/diagnostics")) {
      if (path != "/rinnai/api/state") { send_heavy_cache_(request, path, false); return; }
      std::string copy;
      { std::lock_guard<std::mutex> lock(mutex_); copy = state_; }
      respond_(request, 200, copy);
      return;
    }
    if (request->method() != HTTP_POST) { respond_(request, 405, "{\"error\":\"Metodo nao permitido\"}"); return; }
    if (request->contentLength() > (path == "/rinnai/api/preferences" ? 2048U : 512U)) { respond_(request, 413, "{\"error\":\"Pedido muito grande\"}"); return; }
    if (!same_origin_(request)) { respond_(request, 403, "{\"error\":\"Origem nao permitida\"}"); return; }
    if (path == "/rinnai/api/preferences") {
      heater_health::Settings preferences{};
      preferences.enabled_mask = 0;
      bool valid = true;
      const char *names[]{"favorite1_name", "favorite2_name", "favorite3_name"};
      const char *temperatures[]{"favorite1_temp", "favorite2_temp", "favorite3_temp"};
      for (size_t i = 0; i < 3; ++i) {
        auto name = request->arg(names[i]);
        const auto first = name.find_first_not_of(' '), last = name.find_last_not_of(' ');
        if (first != std::string::npos) name = name.substr(first, last - first + 1);
        if (name.empty() || name.size() > 31 || name.find('\0') != std::string::npos) { valid = false; break; }
        auto &favorite = preferences.favorites[i];
        std::memset(favorite.name, 0, sizeof(favorite.name));
        std::memcpy(favorite.name, name.data(), name.size());
        double temperature = 0;
        if (!parse_(request, temperatures[i], temperature) || temperature != std::floor(temperature) || temperature < 35 || temperature > 50) { valid = false; break; }
        favorite.temperature_c = static_cast<int32_t>(temperature);
      }
      auto set_limit = [&](const char *name, uint32_t &field, double minimum, double maximum) {
        double value = 0;
        if (!parse_(request, name, value) || value != std::floor(value) || value < minimum || value > maximum) { valid = false; return; }
        field = static_cast<uint32_t>(value);
      };
      set_limit("long_use_min", preferences.long_use_min, 1, 180);
      set_limit("continuous_flow_min", preferences.continuous_flow_min, 1, 180);
      set_limit("offline_seconds", preferences.offline_seconds, 10, 600);
      auto set_flag = [&](const char *name, uint32_t flag) {
        double value = 0;
        if (!parse_(request, name, value) || (value != 0 && value != 1)) { valid = false; return; }
        if (value == 1) preferences.enabled_mask |= flag;
      };
      set_flag("long_use_enabled", heater_health::LONG_USE); set_flag("flow_enabled", heater_health::CONTINUOUS_FLOW);
      set_flag("error_enabled", heater_health::HEATER_ERROR); set_flag("offline_enabled", heater_health::OFFLINE);
      if (!valid || !heater_health::valid_settings(preferences)) {
        respond_(request, 400, "{\"error\":\"Confira os favoritos e os limites dos avisos\"}"); return;
      }
      bool accepted = false;
      { std::lock_guard<std::mutex> lock(mutex_);
        if (!preferences_pending_ && !restart_pending_) { preferences_request_ = preferences; preferences_pending_ = true; accepted = true; }
      }
      if (!accepted) { respond_(request, 409, "{\"error\":\"Aguarde a configuracao anterior\"}"); return; }
      respond_(request, 202, "{\"accepted\":true}"); return;
    }
    if (path == "/rinnai/api/temperature") {
      double value = 0;
      if (!parse_(request, "target", value) || value < 35 || value > 50 || value != std::floor(value)) {
        respond_(request, 400, "{\"error\":\"Escolha uma temperatura inteira entre 35 e 50 graus\"}"); return;
      }
      uint32_t command_id = 0, boot_id = 0;
      if (queue_target_(static_cast<int>(value), command_id, boot_id) != 202) {
        respond_(request, 409, "{\"error\":\"Aguarde comunicacao e controle disponiveis\"}"); return;
      }
      respond_(request, 202, "{\"accepted\":true}");
      return;
    }
    if (path == "/rinnai/api/settings") {
      const std::string kind = request->arg("gas_type");
      SettingsRequest settings;
      if ((kind != "GN" && kind != "GLP") || !parse_(request, "gas_tariff", settings.gas_tariff) ||
          !parse_(request, "water_tariff", settings.water_tariff) || settings.gas_tariff < 0 ||
          settings.water_tariff < 0 || settings.gas_tariff > 100000 || settings.water_tariff > 100000) {
        respond_(request, 400, "{\"error\":\"Confira o tipo de gas e as tarifas nao negativas\"}"); return;
      }
      settings.glp = kind == "GLP";
      { std::lock_guard<std::mutex> lock(mutex_);
        if (settings_pending_ || restart_pending_) { respond_(request, 409, "{\"error\":\"Aguarde a configuracao anterior\"}"); return; }
        settings_request_ = settings; settings_pending_ = true;
      }
      respond_(request, 202, "{\"accepted\":true}");
      return;
    }
    respond_(request, 405, "{\"error\":\"Metodo nao permitido\"}");
  }
  int take_target() {
    std::lock_guard<std::mutex> lock(mutex_);
    const int result = target_;
    target_ = 0;
    // Keep the reservation until the main loop has completed or rejected it.
    return result;
  }
  void note_target_started(bool accepted) {
    std::lock_guard<std::mutex> lock(mutex_);
    command_.started(accepted, esphome::millis());
  }
  bool take_cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cancel_pending_) return false;
    cancel_pending_ = false; target_ = 0;
    // A completed readback wins a race with cancellation; never label an
    // already confirmed adjustment as cancelled merely because HTTP was late.
    if (rinnai_smart::controller.busy() || command_.result() == rinnai_api::Result::QUEUED)
      command_.cancel(esphome::millis());
    else
      command_.observe(false, rinnai_smart::controller.succeeded(),
          rinnai_smart::controller.actual_celsius(), rinnai_smart::controller.uncertain(), esphome::millis());
    return true;
  }
  // Called only from the main loop. Delay lets the HTTP acknowledgement leave.
  bool take_restart_request(uint32_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!restart_pending_ || now < 75000U || static_cast<uint32_t>(now - restart_requested_ms_) < 500U ||
        rinnai_smart::controller.busy() || heater_stats::current_session().active) return false;
    restart_pending_ = false; return true;
  }
  bool take_settings(SettingsRequest &request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!settings_pending_) return false;
    request = settings_request_;
    settings_pending_ = false;
    return true;
  }
  bool take_preferences(heater_health::Settings &request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!preferences_pending_) return false;
    request = preferences_request_;
    preferences_pending_ = false;
    return true;
  }
  void refresh(const Live &live, bool update_history) {
    if (update_history) {
      // Free the old buffers BEFORE allocating replacements. Keeping two 48 KB
      // history capacities alive caused allocation aborts on this ESP32.
      // Readers share this mutex and cannot observe the intermediate empty cache.
      std::lock_guard<std::mutex> lock(mutex_);
      if (!heavy_cache_loaned_) {
        std::string{}.swap(history_);
        std::string{}.swap(periods_);
        history_ = history_json(live.uptime_ms);
        periods_ = periods_json();
        auto diagnostics = diagnostics_json();
        if (!diagnostics.empty()) diagnostics_.swap(diagnostics);
      }
    }
    { std::lock_guard<std::mutex> lock(mutex_);
      rinnai_storage::copy_sessions_snapshot(sessions_snapshot_);
      sessions_snapshot_.storage_ok = sessions_snapshot_.storage_ok && live.storage_ok;
      command_.observe(rinnai_smart::controller.busy(), rinnai_smart::controller.succeeded(),
          rinnai_smart::controller.actual_celsius(), rinnai_smart::controller.uncertain(), live.uptime_ms);
      api_water_active_ = heater_stats::current_session().active;
      api_controller_busy_ = rinnai_smart::controller.busy();
      const auto preferences = rinnai_storage::health.settings();
      for (size_t i = 0; i < 3; ++i) api_favorites_[i] = preferences.favorites[i].temperature_c;
      if (target_ == 0 && !rinnai_smart::controller.busy()) target_inflight_ = false;
      targets_ready_ = live.controls_ready && !target_inflight_ && !cancel_pending_ && !restart_pending_;
      Live snapshot = live;
      snapshot.controls_ready = targets_ready_;
      auto state = state_json(snapshot);
      if (!state.empty()) {
        state_.swap(state);
        api_boot_id_ = rinnai_storage::health.boot_count();
        api_sample_ms_ = live.uptime_ms;
        api_sample_epoch_ = rinnai_storage::epoch_now();
      }
    }
  }
 private:
  // ESP-IDF dispatches these requests on its one HTTP task and send() consumes
  // the body synchronously. Transfer the buffer instead of allocating a second
  // history-sized copy. Main-loop refresh skips heavy caches until it is back;
  // the mutex is never held across network I/O or a slow client.
  void send_heavy_cache_(AsyncWebServerRequest *request, const std::string &path, bool require_ready) {
    std::string body;
    std::string *cache = nullptr;
    bool ready = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ready = !require_ready || api_boot_id_ != 0;
      if (ready) {
        cache = path.find("history") != std::string::npos ? &history_ :
                path.find("periods") != std::string::npos ? &periods_ : &diagnostics_;
        body.swap(*cache);
        heavy_cache_loaned_ = true;
      }
    }
    if (!ready) { api_error_(request, 503, "not_ready", "Aguarde a inicializacao dos dados do modulo."); return; }
    respond_(request, 200, body);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      body.swap(*cache);
      heavy_cache_loaned_ = false;
    }
  }
  int queue_target_(int target, uint32_t &id, uint32_t &boot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (target_inflight_ || cancel_pending_ || restart_pending_) return 409;
    if (!targets_ready_ || !api_boot_id_) return 503;
    target_ = target; target_inflight_ = true; targets_ready_ = false;
    boot = api_boot_id_; id = command_.submitted(target, boot, esphome::millis());
    return 202;
  }
  static std::string serialize_(JsonVariantConst value) {
    std::string body; serializeJson(value, body); return body;
  }
  static void api_error_(AsyncWebServerRequest *request, int status, const char *code, const char *message) {
    JsonDocument doc; doc["error"] = message; doc["code"] = code;
    respond_(request, status, serialize_(doc.as<JsonVariantConst>()));
  }
  static bool write_operation_(const std::string &path, rinnai_api_requests::Operation &operation) {
    using Op = rinnai_api_requests::Operation;
    if (path == "/api/v1/temperature") operation = Op::TEMPERATURE;
    else if (path == "/api/v1/favorite") operation = Op::FAVORITE;
    else if (path == "/api/v1/settings") operation = Op::SETTINGS;
    else if (path == "/api/v1/preferences") operation = Op::PREFERENCES;
    else if (path == "/api/v1/control/cancel") operation = Op::CANCEL;
    else if (path == "/api/v1/device/restart") operation = Op::RESTART;
    else return false;
    return true;
  }
  bool read_json_body_(AsyncWebServerRequest *request, std::string &body) {
    const auto content_type = request->get_header("Content-Type");
    std::string mime = content_type ? *content_type : "";
    mime = mime.substr(0, mime.find(';'));
    while (!mime.empty() && mime.back() == ' ') mime.pop_back();
    while (!mime.empty() && mime.front() == ' ') mime.erase(0, 1);
    for (auto &character : mime) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (mime != "application/json") {
      api_error_(request, 415, "unsupported_media_type", "Envie Content-Type: application/json."); return false;
    }
    const size_t length = request->contentLength();
    if (length > rinnai_api_requests::MAX_BODY_BYTES) {
      api_error_(request, 413, "body_too_large", "O corpo JSON deve ter no maximo 2048 bytes."); return false;
    }
    if (!length) { api_error_(request, 400, "invalid_request", "Envie um objeto JSON."); return false; }
    body.resize(length);
    size_t received = 0;
    while (received < length) {
      const int bytes = httpd_req_recv(*request, &body[received], length - received);
      if (bytes <= 0) {
        api_error_(request, bytes == HTTPD_SOCK_ERR_TIMEOUT ? 408 : 400,
            bytes == HTTPD_SOCK_ERR_TIMEOUT ? "body_timeout" : "incomplete_body", "O corpo JSON nao foi recebido por completo.");
        return false;
      }
      received += static_cast<size_t>(bytes);
    }
    return true;
  }
  std::string capabilities_() {
    JsonDocument doc;
    doc["api_version"] = "1"; doc["firmware_version"] = ESPHOME_PROJECT_VERSION;
    doc["model"] = "Rinnai REU-E211"; doc["authentication"] = "none";
    doc["docs"] = "/api"; doc["openapi"] = "/api/openapi.json";
    auto features = doc["capabilities"].to<JsonObject>();
    features["completed_sessions"] = true;
    for (const char *key : {"telemetry", "temperature", "favorites", "tariffs", "alerts", "history", "periods", "diagnostics", "cancel_control", "restart_module"}) features[key] = true;
    features["power_on_off"] = false; features["raw_serial"] = false; features["measured_gas_consumption"] = false;
    auto range = doc["temperature"].to<JsonObject>(); range["minimum"] = 35; range["maximum"] = 50; range["step"] = 1; range["unit"] = "celsius";
    auto units = doc["units"].to<JsonObject>();
    units["temperature"] = "celsius"; units["flow"] = "L/min"; units["power"] = "kW";
    units["energy"] = "kWh"; units["water"] = "L"; units["gas_GN"] = "m3";
    units["gas_GLP"] = "kg"; units["cost"] = "BRL"; units["time"] = "s";
    auto limits = doc["limits"].to<JsonObject>();
    limits["json_body_bytes"] = 2048; limits["pending_temperature_commands"] = 1;
    limits["sessions"] = 24; limits["history_points"] = 720; limits["history_window_s"] = 86400;
    limits["days"] = 31; limits["months"] = 12; limits["favorites"] = 3;
    limits["restart_min_uptime_s"] = 75;
    auto polling = doc["recommended_poll_s"].to<JsonObject>(); polling["state"] = 2; polling["history"] = 10; polling["periods"] = 10; polling["diagnostics"] = 10;
    polling["sessions"] = 10;
    auto native = doc["esphome_native_api"].to<JsonObject>(); native["port"] = 6053; native["encrypted"] = true;
    native["temperature_entity"] = "Temperatura ajustada"; native["action"] = "ajustar_temperatura";
    auto links = doc["endpoints"].to<JsonObject>();
    for (const char *name : {"capabilities", "state", "telemetry", "settings", "control", "history", "sessions", "periods", "diagnostics", "health", "temperature", "favorite", "preferences"})
      links[name] = std::string("/api/v1/") + name;
    links["cancel"] = "/api/v1/control/cancel"; links["restart_module"] = "/api/v1/device/restart";
    return serialize_(doc.as<JsonVariantConst>());
  }
  void read_v1_(AsyncWebServerRequest *request, const std::string &path) {
    if (path == "/api/v1" || path == "/api/v1/" || path == "/api/v1/capabilities") {
      respond_(request, 200, capabilities_()); return;
    }
    if (path == "/api/v1/sessions") {
      std::string body;
      { std::lock_guard<std::mutex> lock(mutex_);
        body = rinnai_sessions::serialize(sessions_snapshot_, session_device_id_, ESPHOME_PROJECT_VERSION);
      }
      if (body.empty()) api_error_(request, 503, "not_ready", "Aguarde um historico com identidade e gravacao confirmadas.");
      else respond_(request, 200, body);
      return;
    }
    if (path == "/api/v1/history" || path == "/api/v1/periods" || path == "/api/v1/diagnostics") {
      send_heavy_cache_(request, path, true); return;
    }
    std::string data;
    rinnai_api::CommandTracker command;
    uint32_t boot = 0, sample = 0;
    uint64_t epoch = 0;
    bool queued = false, cancellation = false, restarting = false, inflight = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      data = state_;
      command = command_; boot = api_boot_id_; sample = api_sample_ms_; epoch = api_sample_epoch_;
      queued = target_ != 0; cancellation = cancel_pending_; restarting = restart_pending_;
      inflight = target_inflight_;
    }
    if (path != "/api/v1/state" && path != "/api/v1/telemetry" && path != "/api/v1/control" &&
        path != "/api/v1/settings" && path != "/api/v1/health") {
      rinnai_api_requests::Operation operation;
      if (write_operation_(path, operation)) api_error_(request, 405, "method_not_allowed", "Use POST para este comando.");
      else api_error_(request, 404, "not_found", "Endpoint inexistente.");
      return;
    }
    if (path == "/api/v1/health" && !boot) {
      JsonDocument starting;
      starting["api_version"] = "1"; starting["status"] = "starting";
      starting["online"] = false; starting["storage_ok"] = false; starting["uptime_s"] = sample / 1000.0;
      starting["version"] = ESPHOME_PROJECT_VERSION; starting["boot_id"] = boot;
      starting["error_code"] = nullptr; starting["restart_pending"] = restarting;
      respond_(request, 503, serialize_(starting.as<JsonVariantConst>())); return;
    }
    JsonDocument doc;
    if (!boot || deserializeJson(doc, data) || !doc.is<JsonObject>()) {
      api_error_(request, 503, "not_ready", "Aguarde a primeira leitura do modulo."); return;
    }
    doc["api_version"] = "1"; doc["sampled_at_uptime_s"] = sample / 1000.0;
    if (epoch) doc["sampled_at_epoch_s"] = epoch; else doc["sampled_at_epoch_s"] = nullptr;
    auto control = doc["control"].as<JsonObject>();
    const bool busy = control["busy"].as<bool>() || command.active() || queued || cancellation || inflight;
    control["api_version"] = "1"; control["boot_id"] = boot;
    if (command.id()) control["command_id"] = command.id(); else control["command_id"] = nullptr;
    control["result"] = command.result_name(); control["busy"] = busy;
    control["ready"] = control["ready"].as<bool>() && !busy && !restarting;
    control["cancel_requested"] = cancellation;
    if (command.id()) {
      control["target_c"] = command.target();
      control["accepted_at_uptime_s"] = command.accepted_ms() / 1000.0;
      control["updated_at_uptime_s"] = command.updated_ms() / 1000.0;
    } else {
      control["target_c"] = doc["target_c"]; control["accepted_at_uptime_s"] = nullptr; control["updated_at_uptime_s"] = nullptr;
    }
    control["setpoint_c"] = doc["setpoint_c"];
    if (command.result() == rinnai_api::Result::QUEUED) control["status"] = "Pedido enfileirado; aguardando o controlador.";
    if (command.active()) doc["target_c"] = command.target();
    if (path == "/api/v1/state") { respond_(request, 200, serialize_(doc.as<JsonVariantConst>())); return; }
    if (path == "/api/v1/control") { respond_(request, 200, serialize_(control)); return; }
    if (path == "/api/v1/settings") { respond_(request, 200, serialize_(doc["settings"])); return; }
    JsonDocument result;
    result["api_version"] = "1";
    if (path == "/api/v1/telemetry") {
      for (const char *name : {"online", "heating", "inlet_c", "outlet_c", "flow_l_min", "power_kcal_min", "power_kw", "setpoint_c", "target_c", "readings", "alerts", "sampled_at_epoch_s", "sampled_at_uptime_s"}) result[name] = doc[name];
      result["error_code"] = doc["diag"]["error_code"];
      if (doc["readings"]["setpoint"]["stale"].as<bool>()) result["setpoint_c"] = nullptr;
      respond_(request, 200, serialize_(result.as<JsonVariantConst>())); return;
    }
    const bool healthy = doc["online"].as<bool>() && doc["diag"]["storage_ok"].as<bool>();
    result["status"] = healthy ? "ok" : "degraded"; result["online"] = doc["online"];
    result["storage_ok"] = doc["diag"]["storage_ok"]; result["uptime_s"] = doc["diag"]["uptime_s"];
    result["version"] = doc["version"]; result["boot_id"] = boot; result["error_code"] = doc["diag"]["error_code"];
    result["restart_pending"] = restarting;
    respond_(request, healthy ? 200 : 503, serialize_(result.as<JsonVariantConst>()));
  }
  void handle_v1_(AsyncWebServerRequest *request, const std::string &path) {
    if (request->method() == HTTP_GET) { read_v1_(request, path); return; }
    using Op = rinnai_api_requests::Operation;
    Op operation;
    if (!write_operation_(path, operation)) {
      const bool resource = path == "/api/v1" || path == "/api/v1/" || path == "/api/v1/capabilities" ||
          path == "/api/v1/state" || path == "/api/v1/telemetry" || path == "/api/v1/control" ||
          path == "/api/v1/history" || path == "/api/v1/sessions" || path == "/api/v1/periods" || path == "/api/v1/diagnostics" || path == "/api/v1/health";
      api_error_(request, resource ? 405 : 404, resource ? "method_not_allowed" : "not_found",
          resource ? "Use GET para este recurso." : "Endpoint inexistente."); return;
    }
    if (request->method() != HTTP_POST) { api_error_(request, 405, "method_not_allowed", "Use POST para este comando."); return; }
    if (!same_origin_(request)) { api_error_(request, 403, "origin_not_allowed", "Origem nao permitida."); return; }
    std::string body;
    if (!read_json_body_(request, body)) return;
    rinnai_api_requests::WriteRequest write;
    std::string error;
    if (!rinnai_api_requests::decode(operation, body, write, error)) {
      api_error_(request, 400, "invalid_request", error.c_str()); return;
    }
    if (operation == Op::TEMPERATURE || operation == Op::FAVORITE) {
      int target = write.target;
      if (operation == Op::FAVORITE) { std::lock_guard<std::mutex> lock(mutex_); target = api_favorites_[write.slot - 1]; }
      uint32_t id = 0, boot = 0;
      const auto status = queue_target_(target, id, boot);
      if (status != 202) {
        api_error_(request, status, status == 409 ? "busy" : "not_ready", "Aguarde comunicacao e controle disponiveis."); return;
      }
      JsonDocument response; response["accepted"] = true; response["boot_id"] = boot;
      response["command_id"] = id; response["target"] = target; response["status_url"] = "/api/v1/control";
      respond_(request, 202, serialize_(response.as<JsonVariantConst>())); return;
    }
    bool accepted = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!api_boot_id_) { api_error_(request, 503, "not_ready", "Aguarde a inicializacao do modulo."); return; }
      if (operation == Op::RESTART && api_sample_ms_ < 75000U) {
        api_error_(request, 503, "not_ready", "Aguarde 75 segundos de funcionamento antes de reiniciar o modulo."); return;
      }
      if (restart_pending_) { api_error_(request, 409, "busy", "Reinicio do modulo ja solicitado."); return; }
      if (operation == Op::SETTINGS && !settings_pending_) {
        settings_request_.glp = write.glp; settings_request_.gas_tariff = write.gas_tariff;
        settings_request_.water_tariff = write.water_tariff; settings_pending_ = true; accepted = true;
      } else if (operation == Op::PREFERENCES && !preferences_pending_) {
        preferences_request_ = write.preferences; preferences_pending_ = true; accepted = true;
      } else if (operation == Op::CANCEL) {
        if (target_inflight_ || command_.active() || api_controller_busy_) {
          cancel_pending_ = true; target_ = 0; accepted = true;
        }
      } else if (operation == Op::RESTART && !target_inflight_ && !command_.active() && !api_controller_busy_ && !api_water_active_ && !cancel_pending_) {
        restart_pending_ = true; restart_requested_ms_ = esphome::millis(); targets_ready_ = false; accepted = true;
      }
    }
    if (!accepted && operation != Op::CANCEL) { api_error_(request, 409, "busy", "Aguarde a operacao anterior ou o fim do uso de agua."); return; }
    JsonDocument response; response["accepted"] = accepted;
    response["status_url"] = operation == Op::CANCEL ? "/api/v1/control" : operation == Op::RESTART ? "/api/v1/health" : "/api/v1/settings";
    if (operation == Op::CANCEL) response["cancelled"] = accepted;
    respond_(request, accepted ? 202 : 200, serialize_(response.as<JsonVariantConst>()));
  }
  static void respond_(AsyncWebServerRequest *request, int code, const std::string &body) {
    // This ESPHome IDF adapter maps unlisted status codes (including 202/400)
    // to 500. Set the HTTP status through IDF after building the content.
    // ESP-IDF sends synchronously. Borrow this live string to avoid another
    // history-sized allocation; the local body remains valid through send().
    auto *response = request->beginResponse(200, "application/json; charset=utf-8",
        reinterpret_cast<const uint8_t *>(body.data()), body.size());
    const char *status = "200 OK";
    switch (code) {
      case 202: status = "202 Accepted"; break;
      case 400: status = "400 Bad Request"; break;
      case 403: status = "403 Forbidden"; break;
      case 404: status = "404 Not Found"; break;
      case 405: status = "405 Method Not Allowed"; break;
      case 408: status = "408 Request Timeout"; break;
      case 409: status = "409 Conflict"; break;
      case 413: status = "413 Payload Too Large"; break;
      case 415: status = "415 Unsupported Media Type"; break;
      case 503: status = "503 Service Unavailable"; break;
    }
    httpd_resp_set_status(*request, status);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  }
  static bool parse_(AsyncWebServerRequest *request, const char *name, double &out) {
    const auto text = request->arg(name);
    if (text.empty() || text.size() > 32) return false;
    char *end = nullptr;
    errno = 0;
    out = std::strtod(text.c_str(), &end);
    return errno == 0 && end == text.c_str() + text.size() && std::isfinite(out);
  }
  static bool same_origin_(AsyncWebServerRequest *request) {
    const auto fetch = request->get_header("Sec-Fetch-Site");
    if (fetch && *fetch == "cross-site") return false;
    const auto origin = request->get_header("Origin");
    if (!origin) return true;  // Local native tools do not send Origin.
    const auto host = request->get_header("Host");
    return host && (*origin == "http://" + *host || *origin == "https://" + *host);
  }
  std::mutex mutex_;
  std::string state_{"{\"online\":false}"};
  std::string history_{"{\"points\":[],\"sessions\":[]}"};
  std::string periods_{"{\"days\":[],\"months\":[],\"clock_valid\":false}"};
  std::string diagnostics_{"{\"boots\":[],\"crash_log\":\"\"}"};
  rinnai_sessions::Snapshot sessions_snapshot_{};
  char session_device_id_[20]{};
  int target_{0};
  bool targets_ready_{false}, target_inflight_{false}, heavy_cache_loaned_{false};
  SettingsRequest settings_request_{};
  heater_health::Settings preferences_request_{};
  bool preferences_pending_{false};
  bool settings_pending_{false}, registered_{false};
  rinnai_api::CommandTracker command_{};
  uint32_t api_boot_id_{0}, api_sample_ms_{0}, restart_requested_ms_{0};
  uint64_t api_sample_epoch_{0};
  int api_favorites_[3]{38, 39, 40};
  bool api_water_active_{false}, api_controller_busy_{false}, cancel_pending_{false}, restart_pending_{false};
};

inline Backend &backend() { static Backend value; return value; }
}  // namespace rinnai_dashboard
