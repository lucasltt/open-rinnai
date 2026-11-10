// Compile the actual Backend class, with only networking/serialization I/O stubbed.
// generated_api_backend.inc is extracted from dashboard_backend.h by the runner.
#include <ArduinoJson.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "../esphome/api_requests.h"
#include "../esphome/api_command.h"
#include "../esphome/heater_control.h"
#include "../esphome/heater_storage.h"

#ifndef ESPHOME_PROJECT_VERSION
#define ESPHOME_PROJECT_VERSION "1.2.0"
#endif

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; std::exit(EXIT_FAILURE); } } while (false)

enum WebRequestMethod { HTTP_GET, HTTP_POST, HTTP_OPTIONS, HTTP_PUT, HTTP_DELETE, HTTP_PATCH, HTTP_HEAD };
inline constexpr int HTTPD_SOCK_ERR_TIMEOUT = -1;
inline constexpr int HTTPD_SOCK_ERR_FAIL = -2;
inline constexpr int ESP_OK = 0;
using esp_err_t = int;

static std::string lower(std::string value) {
  for (auto &c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A');
  return value;
}

struct httpd_req_t {
  std::string body;
  size_t content_len{0}, cursor{0}, max_chunk{static_cast<size_t>(-1)}, recv_calls{0};
  int fail_after{-1}, failure{HTTPD_SOCK_ERR_TIMEOUT};
  uint32_t recv_advance_ms{0};
  unsigned status{0};
  bool closed{false};
  void *handle{this};
  std::map<std::string, std::string> response_headers;
};

static int httpd_req_recv(httpd_req_t *request, char *target, size_t maximum) {
  ++request->recv_calls;
  esphome::test_millis += request->recv_advance_ms;
  if (request->fail_after >= 0 && request->cursor >= static_cast<size_t>(request->fail_after)) return request->failure;
  if (request->cursor >= request->body.size()) return 0;
  size_t size = std::min({maximum, request->max_chunk, request->body.size() - request->cursor});
  if (request->fail_after >= 0) size = std::min(size, static_cast<size_t>(request->fail_after) - request->cursor);
  std::memcpy(target, request->body.data() + request->cursor, size);
  request->cursor += size;
  return static_cast<int>(size);
}
static esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status) {
  request->status = static_cast<unsigned>(std::atoi(status)); return ESP_OK;
}
static int httpd_req_to_sockfd(httpd_req_t *) { return 42; }
static esp_err_t httpd_sess_trigger_close(void *handle, int) {
  static_cast<httpd_req_t *>(handle)->closed = true; return ESP_OK;
}
inline uint32_t millis() { return esphome::millis(); }

class AsyncWebServerResponse {
 public:
  AsyncWebServerResponse(httpd_req_t *request, const uint8_t *data, size_t size)
      : request_(request), data_(data), size_(size) {}
  void addHeader(const char *name, const char *value) { request_->response_headers[lower(name)] = value; }
  const uint8_t *data() const { return data_; }
  size_t size() const { return size_; }
 private:
  httpd_req_t *request_;
  const uint8_t *data_;
  size_t size_;
};

class AsyncWebServerRequest {
 public:
  AsyncWebServerRequest(std::string path, WebRequestMethod method = HTTP_GET, std::string body = {})
      : path_(std::move(path)), method_(method) {
    raw.body = std::move(body); raw.content_len = raw.body.size();
    headers["host"] = "192.168.1.50";
    headers["content-type"] = "application/json";
    headers["content-length"] = std::to_string(raw.content_len);
  }
  std::string url() const { return path_; }
  WebRequestMethod method() const { return method_; }
  size_t contentLength() const { return raw.content_len; }
  std::optional<std::string> get_header(const char *name) const {
    const auto found = headers.find(lower(name));
    return found == headers.end() ? std::nullopt : std::make_optional(found->second);
  }
  bool hasHeader(const char *name) const { return get_header(name).has_value(); }
  std::string arg(const char *name) {
    const auto found = arguments.find(name); return found == arguments.end() ? "" : found->second;
  }
  bool hasArg(const char *name) { return arguments.count(name) != 0; }
  operator httpd_req_t *() { return &raw; }
  AsyncWebServerResponse *beginResponse(int code, const char *type, const uint8_t *data, size_t size) {
    // Model the IDF adapter's real narrow initial status-code mapping.
    raw.status = code == 200 || code == 404 || code == 409 ? static_cast<unsigned>(code) : 500;
    raw.response_headers["content-type"] = type;
    return new AsyncWebServerResponse(&raw, data, size);
  }
  void send(AsyncWebServerResponse *response) {
    response_.reset(response);
    if (during_send) during_send(response->data(), response->size());
    sent.assign(reinterpret_cast<const char *>(response->data()), response->size());
  }
  httpd_req_t raw;
  std::map<std::string, std::string> headers, arguments;
  std::string sent;
  std::function<void(const uint8_t *, size_t)> during_send;
 private:
  std::string path_;
  WebRequestMethod method_;
  std::unique_ptr<AsyncWebServerResponse> response_;
};

class AsyncWebHandler {
 public:
  virtual ~AsyncWebHandler() = default;
  virtual bool canHandle(AsyncWebServerRequest *) const { return false; }
  virtual void handleRequest(AsyncWebServerRequest *) {}
};

namespace esphome {
namespace web_server_base {
struct Base { unsigned registrations{0}; void add_handler(AsyncWebHandler *) { ++registrations; } };
inline Base *global_web_server_base = nullptr;
}
namespace json {
template<typename Callback> std::string build_json(Callback callback) {
  JsonDocument document; callback(document.to<JsonObject>());
  std::string result; serializeJson(document, result); return result;
}
}
}
namespace rinnai_smart { inline rinnai_control::Controller controller{}; }

struct StubAsset { const char *path; const char *type; const uint8_t *data; size_t size; bool compressed; };
namespace rinnai_pwa { inline const StubAsset *find(const char *) { return nullptr; } }
namespace rinnai_api_assets { inline const StubAsset *find(const char *) { return nullptr; } }
namespace rinnai_favicon { inline constexpr char SVG[] = "<svg/>"; inline constexpr uint8_t ICON[]{1, 2}; }

namespace rinnai_dashboard {
// The runner inserts the current Live and SettingsRequest definitions here.
#include "generated_api_structs.inc"
#include "generated_api_snapshot.inc"
inline bool fixture_setpoint_stale = false;
inline bool fixture_state_failure = false;
inline unsigned fixture_heavy_revision = 0, fixture_heavy_builds = 0;
inline bool fixture_diagnostics_failure = false;
inline const char *fixture_history_address = nullptr;
inline const char *fixture_periods_address = nullptr;
inline const char *fixture_diagnostics_address = nullptr;

inline std::string state_json(const Live &live) {
  if (fixture_state_failure) return {};
  return esphome::json::build_json([&](JsonObject root) {
    root["version"] = live.version; root["online"] = live.online; root["heating"] = live.heating;
    root["inlet_c"] = live.inlet_c; root["outlet_c"] = live.outlet_c;
    root["flow_l_min"] = live.flow_l_min; root["power_kw"] = live.power_kcal_min * 0.06978;
    root["setpoint_c"] = rinnai_smart::controller.actual_celsius();
    root["target_c"] = rinnai_smart::controller.desired();
    auto readings = root["readings"].to<JsonObject>();
    readings["setpoint"]["age_s"] = fixture_setpoint_stale ? 16 : 1;
    readings["setpoint"]["stale"] = fixture_setpoint_stale;
    auto control = root["control"].to<JsonObject>();
    control["busy"] = rinnai_smart::controller.busy(); control["ready"] = live.controls_ready;
    control["uncertain"] = rinnai_smart::controller.uncertain();
    control["status"] = rinnai_smart::controller.status();
    control["confirmed_steps"] = rinnai_smart::controller.confirmed_steps();
    auto settings = root["settings"].to<JsonObject>();
    const auto tariffs = heater_stats::settings();
    settings["gas_type"] = tariffs.gas_type == heater_stats::GasType::GN ? "GN" : "GLP";
    settings["gas_tariff"] = tariffs.gas_tariff_per_unit;
    settings["water_tariff"] = tariffs.water_tariff_per_m3;
    auto favorites = settings["favorites"].to<JsonArray>();
    for (const auto &favorite : rinnai_storage::health.settings().favorites) {
      auto row = favorites.add<JsonObject>(); row["name"] = std::string(favorite.name); row["temp_c"] = favorite.temperature_c;
    }
    auto diag = root["diag"].to<JsonObject>();
    diag["uptime_s"] = live.uptime_ms / 1000.0; diag["error_code"] = live.error_code;
    diag["boot_count"] = rinnai_storage::health.boot_count(); diag["storage_ok"] = live.storage_ok;
  });
}
inline std::string fixture_heavy(const char *prefix, const char *&address, size_t bytes) {
  ++fixture_heavy_builds;
  std::string result = std::string(prefix) + ",\"revision\":" + std::to_string(fixture_heavy_revision) + ",\"padding\":\"" + std::string(bytes, 'x') + "\"}";
  address = result.data();
  return result;
}
inline std::string history_json(uint32_t) { return fixture_heavy("{\"samples\":[],\"sessions\":[],\"session_capacity\":24", fixture_history_address, 34000); }
inline std::string periods_json() { return fixture_heavy("{\"clock_valid\":true,\"days\":[],\"months\":[]", fixture_periods_address, 5000); }
inline std::string diagnostics_json() {
  if (fixture_diagnostics_failure) return {};
  return fixture_heavy("{\"storage_ok\":true,\"boot_count\":1,\"crash_count\":0", fixture_diagnostics_address, 3000);
}
#include "generated_api_backend.inc"
}

static void setup() {
  static esphome::ESPPreferences preferences;
  preferences.durable.clear(); preferences.pending.clear();
  preferences.allow_save = preferences.allow_sync = true;
  esphome::global_preferences = &preferences;
  esphome::test_millis = 1000;
  heater_stats::detail::accounting = heater_stats::detail::Accounting{};
  rinnai_storage::initialized = false; rinnai_storage::begin(3);
  rinnai_smart::controller = rinnai_control::Controller{};
  rinnai_smart::controller.update_observation(true, true, 0, 1000, 1000);
  rinnai_smart::controller.reading(0x060E, 6, 1000);
  rinnai_dashboard::fixture_setpoint_stale = false;
  rinnai_dashboard::fixture_state_failure = false;
  rinnai_dashboard::fixture_diagnostics_failure = false;
  rinnai_dashboard::fixture_heavy_revision = 0;
  rinnai_dashboard::fixture_heavy_builds = 0;
}
static rinnai_dashboard::Live live() {
  rinnai_dashboard::Live current;
  current.version = ESPHOME_PROJECT_VERSION; current.online = current.storage_ok = true;
  current.controls_ready = !rinnai_smart::controller.busy();
  current.inlet_c = 20.5; current.outlet_c = 38; current.flow_l_min = 0; current.power_kcal_min = 0;
  current.uptime_ms = esphome::millis(); current.error_code = 0;
  return current;
}
static JsonDocument response(AsyncWebServerRequest &request, rinnai_dashboard::Backend &backend, unsigned status) {
  CHECK(backend.canHandle(&request)); backend.handleRequest(&request);
  CHECK(request.raw.status == status);
  CHECK(request.raw.response_headers["cache-control"] == "no-store");
  JsonDocument document; CHECK(!deserializeJson(document, request.sent)); return document;
}

static void legacy_smoke() {
  setup(); rinnai_dashboard::Backend backend; backend.refresh(live(), true);
  AsyncWebServerRequest get("/rinnai/api/state");
  auto state = response(get, backend, 200);
  CHECK(state["online"].as<bool>() && state["setpoint_c"].as<int>() == 38);
  for (const char *path : {"/rinnai/api/history", "/rinnai/api/periods", "/rinnai/api/diagnostics"}) {
    AsyncWebServerRequest request(path); response(request, backend, 200);
  }
  AsyncWebServerRequest invalid("/rinnai/api/temperature", HTTP_POST);
  invalid.headers["content-type"] = "application/x-www-form-urlencoded";
  invalid.arguments["target"] = "99";
  response(invalid, backend, 400);
  CHECK(backend.take_target() == 0);
  AsyncWebServerRequest target("/rinnai/api/temperature", HTTP_POST);
  target.headers["content-type"] = "application/x-www-form-urlencoded"; target.arguments["target"] = "39";
  response(target, backend, 202);
  CHECK(backend.take_target() == 39);
  AsyncWebServerRequest conflict("/rinnai/api/temperature", HTTP_POST);
  conflict.arguments["target"] = "40";
  response(conflict, backend, 409);
  AsyncWebServerRequest foreign("/rinnai/api/settings", HTTP_POST);
  foreign.headers["origin"] = "http://example.invalid";
  response(foreign, backend, 403);
  AsyncWebServerRequest method("/rinnai/api/temperature", HTTP_GET);
  response(method, backend, 405);
}

static void error(AsyncWebServerRequest &request, rinnai_dashboard::Backend &backend, unsigned status, const char *code) {
  const auto document = response(request, backend, status);
  CHECK(document["error"].is<const char *>());
  CHECK(document["code"].as<std::string>() == code);
}

static void discovery_and_reads() {
  setup(); rinnai_dashboard::Backend cold;
  for (const auto *path : {"/api/v1", "/api/v1/", "/api/v1/capabilities"}) {
    AsyncWebServerRequest request(path); const auto document = response(request, cold, 200);
    CHECK(document["api_version"].as<std::string>() == "1");
    CHECK(document["temperature"]["minimum"].as<int>() == 35);
    CHECK(document["temperature"]["maximum"].as<int>() == 50);
    CHECK(!document["capabilities"]["power_on_off"].as<bool>());
    CHECK(document["capabilities"]["temperature"].as<bool>());
    CHECK(document["esphome_native_api"]["encrypted"].as<bool>());
  }
  AsyncWebServerRequest early("/api/v1/state"); error(early, cold, 503, "not_ready");
  for (const auto *path : {"/api/v1/history", "/api/v1/periods", "/api/v1/diagnostics"}) {
    AsyncWebServerRequest request(path); error(request, cold, 503, "not_ready");
  }
  AsyncWebServerRequest starting("/api/v1/health"); const auto starting_health = response(starting, cold, 503);
  CHECK(starting_health["status"].as<std::string>() == "starting");
  CHECK(!starting_health["online"].as<bool>() && starting_health["boot_id"].as<int>() == 0);
  AsyncWebServerRequest unknown("/api/v1/missing"); error(unknown, cold, 404, "not_found");
  AsyncWebServerRequest future("/api/v2/state"); error(future, cold, 404, "not_found");
  AsyncWebServerRequest wrong_method("/api/v1/state", HTTP_POST, "{}");
  error(wrong_method, cold, 405, "method_not_allowed");
  AsyncWebServerRequest wrong_get("/api/v1/temperature"); error(wrong_get, cold, 405, "method_not_allowed");
  cold.refresh(live(), true);
  for (const auto *path : {"/api/v1/state", "/api/v1/telemetry", "/api/v1/control", "/api/v1/settings",
       "/api/v1/history", "/api/v1/periods", "/api/v1/diagnostics", "/api/v1/health"}) {
    AsyncWebServerRequest request(path); response(request, cold, 200);
  }
  AsyncWebServerRequest control("/api/v1/control"); auto document = response(control, cold, 200);
  CHECK(document["result"].as<std::string>() == "idle" && document["command_id"].isNull());
  CHECK(document["boot_id"].as<unsigned>() == 1 && document["ready"].as<bool>());
  rinnai_dashboard::fixture_setpoint_stale = true; cold.refresh(live(), false);
  AsyncWebServerRequest stale("/api/v1/telemetry"); document = response(stale, cold, 200);
  CHECK(document["setpoint_c"].isNull() && document["readings"]["setpoint"]["stale"].as<bool>());
  auto offline = live(); offline.online = false; cold.refresh(offline, false);
  AsyncWebServerRequest health("/api/v1/health"); document = response(health, cold, 503);
  CHECK(document["status"].as<std::string>() == "degraded");
  CHECK(!document["online"].as<bool>());
}

static void body_guards_and_queues() {
  setup(); rinnai_dashboard::Backend backend; backend.refresh(live(), true);
  AsyncWebServerRequest missing_type("/api/v1/temperature", HTTP_POST, "{\"target\":38}");
  missing_type.headers.erase("content-type"); error(missing_type, backend, 415, "unsupported_media_type");
  CHECK(missing_type.raw.recv_calls == 0 && backend.take_target() == 0);
  AsyncWebServerRequest form("/api/v1/temperature", HTTP_POST, "target=38");
  form.headers["content-type"] = "application/x-www-form-urlencoded";
  error(form, backend, 415, "unsupported_media_type");
  AsyncWebServerRequest huge("/api/v1/temperature", HTTP_POST, std::string(2049, ' '));
  error(huge, backend, 413, "body_too_large"); CHECK(huge.raw.recv_calls == 0);
  AsyncWebServerRequest empty("/api/v1/temperature", HTTP_POST);
  error(empty, backend, 400, "invalid_request");
  AsyncWebServerRequest truncated("/api/v1/temperature", HTTP_POST, "{\"target\":38}");
  truncated.raw.content_len += 4;
  error(truncated, backend, 400, "incomplete_body"); CHECK(backend.take_target() == 0);
  AsyncWebServerRequest timeout("/api/v1/temperature", HTTP_POST, "{\"target\":38}");
  timeout.raw.fail_after = 4; timeout.raw.max_chunk = 2;
  error(timeout, backend, 408, "body_timeout"); CHECK(timeout.raw.recv_calls == 3 && backend.take_target() == 0);
  for (const char *body : {"{\"target\":true}", "{\"target\":34}", "{\"target\":38,\"target\":39}", "[]", "{target:38}"}) {
    AsyncWebServerRequest request("/api/v1/temperature", HTTP_POST, body);
    error(request, backend, 400, "invalid_request"); CHECK(backend.take_target() == 0);
  }
  AsyncWebServerRequest origin("/api/v1/temperature", HTTP_POST, "{\"target\":38}");
  origin.headers["origin"] = "https://example.invalid";
  error(origin, backend, 403, "origin_not_allowed"); CHECK(origin.raw.recv_calls == 0);
  AsyncWebServerRequest cross_site("/api/v1/temperature", HTTP_POST, "{\"target\":38}");
  cross_site.headers["origin"] = "https://192.168.1.50"; cross_site.headers["sec-fetch-site"] = "cross-site";
  error(cross_site, backend, 403, "origin_not_allowed");
  AsyncWebServerRequest put("/api/v1/temperature", HTTP_PUT, "{\"target\":38}");
  error(put, backend, 405, "method_not_allowed");

  AsyncWebServerRequest target("/api/v1/temperature", HTTP_POST, "{\"target\":38.0}");
  target.headers["content-type"] = " Application/JSON ; charset=utf-8";
  target.headers["origin"] = "https://192.168.1.50"; target.raw.max_chunk = 2;
  auto document = response(target, backend, 202);
  CHECK(target.raw.recv_calls > 1 && target.raw.cursor == target.raw.content_len);
  CHECK(document["accepted"].as<bool>() && document["command_id"].as<int>() == 1);
  CHECK(document["boot_id"].as<int>() == 1 && document["target"].as<int>() == 38);
  AsyncWebServerRequest queued("/api/v1/control"); document = response(queued, backend, 200);
  CHECK(document["busy"].as<bool>() && !document["ready"].as<bool>());
  CHECK(document["result"].as<std::string>() == "queued");
  AsyncWebServerRequest conflicting("/api/v1/temperature", HTTP_POST, "{\"target\":39}");
  error(conflicting, backend, 409, "busy");
  CHECK(backend.take_target() == 38);
  const bool accepted = rinnai_smart::controller.target_request(38, esphome::millis());
  CHECK(accepted); backend.note_target_started(accepted);
  AsyncWebServerRequest running("/api/v1/control"); document = response(running, backend, 200);
  CHECK(document["result"].as<std::string>() == "running");
  const auto action = rinnai_smart::controller.next_action(esphome::millis());
  CHECK(action.kind == rinnai_control::Kind::QUERY_INDEX);
  rinnai_smart::controller.on_sent(action, true, esphome::millis());
  esphome::test_millis += 1; rinnai_smart::controller.reading(0x060E, 6, esphome::millis());
  CHECK(rinnai_smart::controller.succeeded());
  backend.refresh(live(), false);
  AsyncWebServerRequest confirmed("/api/v1/control"); document = response(confirmed, backend, 200);
  CHECK(document["result"].as<std::string>() == "confirmed" && !document["busy"].as<bool>());
  CHECK(document["setpoint_c"].as<int>() == 38 && document["command_id"].as<int>() == 1);
  AsyncWebServerRequest favorite("/api/v1/favorite", HTTP_POST, "{\"slot\":2}");
  document = response(favorite, backend, 202);
  CHECK(document["command_id"].as<int>() == 2 && document["target"].as<int>() == 39);
  CHECK(backend.take_target() == 39); backend.note_target_started(false); backend.refresh(live(), false);
  AsyncWebServerRequest failed("/api/v1/control"); document = response(failed, backend, 200);
  CHECK(document["result"].as<std::string>() == "failed");
}

static void settings_cancel_restart() {
  setup(); rinnai_dashboard::Backend cold;
  AsyncWebServerRequest early("/api/v1/device/restart", HTTP_POST, "{\"confirm\":true}");
  error(early, cold, 503, "not_ready");
  cold.refresh(live(), true);
  AsyncWebServerRequest too_soon("/api/v1/device/restart", HTTP_POST, "{\"confirm\":true}");
  error(too_soon, cold, 503, "not_ready");
  CHECK(!cold.take_restart_request(74999));
  AsyncWebServerRequest cancel_idle("/api/v1/control/cancel", HTTP_POST, "{}");
  auto document = response(cancel_idle, cold, 200);
  CHECK(!document["cancelled"].as<bool>() && !cold.take_cancel());
  AsyncWebServerRequest queued("/api/v1/temperature", HTTP_POST, "{\"target\":39}");
  response(queued, cold, 202);
  AsyncWebServerRequest cancel("/api/v1/control/cancel", HTTP_POST, "{}");
  document = response(cancel, cold, 202);
  CHECK(document["cancelled"].as<bool>() && cold.take_target() == 0);
  AsyncWebServerRequest cancelled("/api/v1/control"); document = response(cancelled, cold, 200);
  CHECK(document["result"].as<std::string>() == "queued" && document["cancel_requested"].as<bool>());
  CHECK(cold.take_cancel() && !cold.take_cancel());
  AsyncWebServerRequest applied_cancel("/api/v1/control"); document = response(applied_cancel, cold, 200);
  CHECK(document["result"].as<std::string>() == "cancelled" && !document["cancel_requested"].as<bool>());
  CHECK(document["busy"].as<bool>() && !document["ready"].as<bool>());
  cold.refresh(live(), false);
  AsyncWebServerRequest settings("/api/v1/settings", HTTP_POST, "{\"gas_type\":\"GLP\",\"gas_tariff\":9,\"water_tariff\":2}");
  response(settings, cold, 202);
  AsyncWebServerRequest repeated("/api/v1/settings", HTTP_POST, "{\"gas_type\":\"GN\",\"gas_tariff\":1,\"water_tariff\":1}");
  error(repeated, cold, 409, "busy");
  rinnai_dashboard::SettingsRequest selected;
  CHECK(cold.take_settings(selected));
  CHECK(selected.glp && selected.gas_tariff == 9 && selected.water_tariff == 2);
  CHECK(!cold.take_settings(selected));
  const char *preferences = R"({"favorites":[{"name":"Meu banho","temp_c":37},{"name":"Conforto","temp_c":39},{"name":"Cozinha","temp_c":42}],"alerts":{"long_use_enabled":true,"flow_enabled":false,"error_enabled":true,"offline_enabled":true,"long_use_min":12,"continuous_flow_min":25,"offline_seconds":45}})";
  AsyncWebServerRequest prefs("/api/v1/preferences", HTTP_POST, preferences);
  response(prefs, cold, 202);
  AsyncWebServerRequest duplicate_prefs("/api/v1/preferences", HTTP_POST, preferences);
  error(duplicate_prefs, cold, 409, "busy");
  heater_health::Settings configured;
  CHECK(cold.take_preferences(configured) && !cold.take_preferences(configured));
  CHECK(std::string(configured.favorites[0].name) == "Meu banho" && configured.favorites[0].temperature_c == 37);
  CHECK(configured.continuous_flow_min == 25 && configured.offline_seconds == 45);
  CHECK((configured.enabled_mask & heater_health::CONTINUOUS_FLOW) == 0);

  esphome::test_millis = 76000;
  rinnai_smart::controller.update_observation(true, true, 0, 76000, 76000);
  rinnai_smart::controller.reading(0x060E, 6, 76000);
  heater_stats::observe(heater_stats::Signal::FLOW_L_MIN, 4, 76000); heater_stats::tick();
  CHECK(heater_stats::current_session().active);
  cold.refresh(live(), false);
  AsyncWebServerRequest water("/api/v1/device/restart", HTTP_POST, "{\"confirm\":true}");
  error(water, cold, 409, "busy");
  esphome::test_millis = 76001;
  heater_stats::observe(heater_stats::Signal::FLOW_L_MIN, 0, 76001);
  esphome::test_millis = 76002; heater_stats::tick();
  CHECK(!heater_stats::current_session().active);
  cold.refresh(live(), false);
  CHECK(rinnai_smart::controller.target_request(39, esphome::millis()));
  cold.refresh(live(), false);
  AsyncWebServerRequest active("/api/v1/device/restart", HTTP_POST, "{\"confirm\":true}");
  error(active, cold, 409, "busy");
  rinnai_smart::controller.cancel(); cold.refresh(live(), false);
  const auto restart_at = esphome::millis();
  AsyncWebServerRequest restart("/api/v1/device/restart", HTTP_POST, "{\"confirm\":true}");
  document = response(restart, cold, 202);
  CHECK(document["accepted"].as<bool>() && !cold.take_restart_request(restart_at + 499));
  AsyncWebServerRequest waiting("/api/v1/health"); document = response(waiting, cold, 200);
  CHECK(document["restart_pending"].as<bool>());
  AsyncWebServerRequest double_restart("/api/v1/device/restart", HTTP_POST, "{\"confirm\":true}");
  error(double_restart, cold, 409, "busy");
  CHECK(cold.take_restart_request(restart_at + 500) && !cold.take_restart_request(restart_at + 501));
}

static void late_cancel_preserves_confirmation() {
  setup(); rinnai_dashboard::Backend backend; backend.refresh(live(), true);
  AsyncWebServerRequest queued("/api/v1/temperature", HTTP_POST, "{\"target\":38}");
  response(queued, backend, 202);
  CHECK(backend.take_target() == 38 && rinnai_smart::controller.target_request(38, 1000));
  backend.note_target_started(true); backend.refresh(live(), false);
  const auto action = rinnai_smart::controller.next_action(1000);
  rinnai_smart::controller.on_sent(action, true, 1000);
  rinnai_smart::controller.reading(0x060E, 6, 1001);
  CHECK(rinnai_smart::controller.succeeded());
  // Completion reaches the controller before the next API cache refresh.
  AsyncWebServerRequest cancel("/api/v1/control/cancel", HTTP_POST, "{}");
  response(cancel, backend, 202);
  CHECK(backend.take_cancel());
  AsyncWebServerRequest finished("/api/v1/control"); const auto document = response(finished, backend, 200);
  CHECK(document["result"].as<std::string>() == "confirmed");
  CHECK(rinnai_smart::controller.succeeded());
}

static void snapshot_serializer() {
  JsonDocument document;
  document["payload"] = std::string(7000, 'z');
  auto serialized = rinnai_dashboard::serialize_snapshot(document);
  CHECK(serialized.size() == measureJson(document));
  CHECK(serialized.size() > 5120);  // No SDK serialization truncation limit.
  JsonDocument parsed;
  CHECK(!deserializeJson(parsed, serialized));
  CHECK(parsed["payload"].as<std::string>().size() == 7000);
  class RejectAllocator : public ArduinoJson::Allocator {
   public:
    void *allocate(size_t) override { return nullptr; }
    void deallocate(void *) override {}
    void *reallocate(void *, size_t) override { return nullptr; }
  } rejected;
  JsonDocument failed(&rejected);
  failed["payload"] = std::string(2000, 'z');
  CHECK(failed.overflowed());
  CHECK(rinnai_dashboard::serialize_snapshot(failed).empty());
}

static void cache_loans_and_snapshot_failure() {
  for (const auto *path : {"/rinnai/api/history", "/rinnai/api/periods", "/rinnai/api/diagnostics",
                         "/api/v1/history", "/api/v1/periods", "/api/v1/diagnostics"}) {
    setup(); rinnai_dashboard::Backend backend;
    rinnai_dashboard::fixture_heavy_revision = 1;
    backend.refresh(live(), true);
    const unsigned built = rinnai_dashboard::fixture_heavy_builds;
    const auto address = std::string(path).find("history") != std::string::npos ? rinnai_dashboard::fixture_history_address :
                         std::string(path).find("periods") != std::string::npos ? rinnai_dashboard::fixture_periods_address : rinnai_dashboard::fixture_diagnostics_address;
    AsyncWebServerRequest request(path);
    request.during_send = [&](const uint8_t *data, size_t size) {
      CHECK(reinterpret_cast<const char *>(data) == address);  // Loan, no body allocation/copy.
      const std::string before(reinterpret_cast<const char *>(data), size);
      rinnai_dashboard::fixture_heavy_revision = 2;
      esphome::test_millis += 1000;
      backend.refresh(live(), true);  // Simulated main loop while network send blocks.
      CHECK(rinnai_dashboard::fixture_heavy_builds == built);
      CHECK(std::string(reinterpret_cast<const char *>(data), size) == before);
    };
    auto document = response(request, backend, 200);
    CHECK(document["revision"].as<unsigned>() == 1);
    AsyncWebServerRequest restored(path);
    document = response(restored, backend, 200);
    CHECK(document["revision"].as<unsigned>() == 1);
    backend.refresh(live(), true);
    CHECK(rinnai_dashboard::fixture_heavy_builds == built + 3);
    AsyncWebServerRequest refreshed(path);
    document = response(refreshed, backend, 200);
    CHECK(document["revision"].as<unsigned>() == 2);
  }
  setup(); rinnai_dashboard::Backend backend; backend.refresh(live(), true);
  AsyncWebServerRequest before("/api/v1/state"); auto document = response(before, backend, 200);
  const auto sampled = document["sampled_at_uptime_s"].as<double>();
  rinnai_dashboard::fixture_state_failure = true;
  rinnai_dashboard::fixture_diagnostics_failure = true;
  esphome::test_millis += 1000;
  backend.refresh(live(), true);
  AsyncWebServerRequest preserved("/api/v1/state"); document = response(preserved, backend, 200);
  CHECK(document["online"].as<bool>() && document["sampled_at_uptime_s"].as<double>() == sampled);
  AsyncWebServerRequest diagnostics("/api/v1/diagnostics"); document = response(diagnostics, backend, 200);
  CHECK(document["storage_ok"].as<bool>() && document["revision"].as<unsigned>() == 0);
}

static void completed_sessions_durable_endpoint() {
  setup();
  rinnai_dashboard::Backend backend;
  esphome::web_server_base::Base base;
  esphome::web_server_base::global_web_server_base = &base;
  CHECK(backend.begin());
  backend.refresh(live(), false);
  AsyncWebServerRequest cold("/api/v1/sessions"); error(cold, backend, 503, "not_ready");
  CHECK(rinnai_storage::save_totals(true));
  backend.refresh(live(), false);
  AsyncWebServerRequest empty("/api/v1/sessions"); auto doc = response(empty, backend, 200);
  CHECK(doc["schema_version"].as<int>() == 1 && doc["sessions"].size() == 0);
  CHECK(doc["device_id"].as<std::string>() == "rinnai-020000000001");
  CHECK(doc["firmware_version"].as<std::string>() == "1.2.0");
  CHECK(doc["oldest_sequence"].isNull() && doc["newest_sequence"].isNull());
  const auto stream = doc["stream_id"].as<std::string>();
  CHECK(stream.size() == 32 && doc["ready"].as<bool>() && doc["storage_ok"].as<bool>());
  heater_stats::Session ended;
  ended.sequence = 1; ended.start_ms = 1000; ended.end_ms = 2000; ended.duration_ms = 1000;
  ended.heating_seconds = 1; ended.water_l = 1.2; ended.energy_kwh = .03;
  ended.complete = true; ended.end_reason = heater_stats::EndReason::FLOW_STOP;
  esphome::test_millis = 3000;
  CHECK(heater_stats::set_counters({1.2, .03, 1, 1}));
  CHECK(rinnai_storage::history.add(ended, 3000, 1793000000ULL));
  backend.refresh(live(), false);
  AsyncWebServerRequest pending("/api/v1/sessions"); doc = response(pending, backend, 200);
  CHECK(doc["total_sessions"].as<unsigned>() == 0 && doc["sessions"].size() == 0);
  esphome::global_preferences->allow_sync = false;
  CHECK(!rinnai_storage::save_totals(true));
  backend.refresh(live(), false);
  AsyncWebServerRequest failed("/api/v1/sessions"); error(failed, backend, 503, "not_ready");
  esphome::global_preferences->allow_sync = true;
  CHECK(rinnai_storage::save_totals(true));
  backend.refresh(live(), false);
  for (unsigned repetition = 0; repetition < 5; ++repetition) {
    AsyncWebServerRequest request("/api/v1/sessions");
    request.during_send = [&](const uint8_t *, size_t) {
      esphome::test_millis += 1000;
      backend.refresh(live(), false);  // Network send does not hold the mutex.
    };
    doc = response(request, backend, 200);
    CHECK(doc["stream_id"].as<std::string>() == stream);
    CHECK(doc["sessions"].size() == 1 && doc["total_sessions"].as<unsigned>() == 1);
    CHECK(doc["sessions"][0]["id"].as<std::string>() == stream + ":1");
    CHECK(doc["sessions"][0]["heating_s"].as<int>() == 1);
    CHECK(doc["sessions"][0]["water_cost"].isNull() && doc["sessions"][0]["gas_cost"].isNull());
  }
  AsyncWebServerRequest capabilities("/api/v1/capabilities");doc = response(capabilities, backend, 200);
  CHECK(doc["capabilities"]["completed_sessions"].as<bool>());
  CHECK(doc["endpoints"]["sessions"].as<std::string>() == "/api/v1/sessions");
  CHECK(doc["recommended_poll_s"]["sessions"].as<int>() == 10);
  AsyncWebServerRequest write("/api/v1/sessions", HTTP_POST, "{}"); error(write, backend, 405, "method_not_allowed");
  CHECK(write.raw.recv_calls == 0);
  esphome::web_server_base::global_web_server_base = nullptr;
}

int main() {
  completed_sessions_durable_endpoint();
  snapshot_serializer();
  cache_loans_and_snapshot_failure();
  legacy_smoke();
  discovery_and_reads();
  body_guards_and_queues();
  settings_cancel_restart();
  late_cancel_preserves_confirmation();
  std::cout << "API Backend dispatch: " << checks << " checks passed\n";
}
