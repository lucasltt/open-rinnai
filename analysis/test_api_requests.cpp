// Host-only API input validation. No networking or heater writes.
#include "../esphome/api_requests.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

using rinnai_api_requests::Operation;
using rinnai_api_requests::WriteRequest;
using rinnai_api_requests::decode;
static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; std::exit(EXIT_FAILURE); } } while (false)

static WriteRequest accept(Operation operation, const std::string &body) {
  WriteRequest result;
  std::string error = "old error";
  CHECK(decode(operation, body, result, error));
  CHECK(error.empty());
  return result;
}

static void reject(Operation operation, const std::string &body) {
  WriteRequest result;
  result.target = 123; result.slot = 44; result.confirm = true; result.gas_tariff = 123.75;
  result.preferences.enabled_mask = 5;
  std::string error;
  CHECK(!decode(operation, body, result, error));
  CHECK(!error.empty());
  CHECK(result.target == 123 && result.slot == 44 && result.confirm);
  CHECK(result.gas_tariff == 123.75 && result.preferences.enabled_mask == 5);
}

static const char *PREFERENCES = R"({"favorites":[{"name":"Banho","temp_c":38},{"name":"Conforto","temp_c":39},{"name":"Cozinha","temp_c":40}],"alerts":{"long_use_enabled":true,"flow_enabled":true,"error_enabled":true,"offline_enabled":true,"long_use_min":15,"continuous_flow_min":20,"offline_seconds":30}})";

static JsonDocument preferences() {
  JsonDocument result;
  CHECK(!deserializeJson(result, PREFERENCES));
  return result;
}

static std::string json(const JsonDocument &document) {
  std::string result;
  serializeJson(document, result);
  return result;
}

static void targets_and_favorites() {
  for (int target = 35; target <= 50; ++target) {
    const auto body = "{\"target\":" + std::to_string(target) + "}";
    CHECK(accept(Operation::TEMPERATURE, body).target == target);
  }
  CHECK(accept(Operation::TEMPERATURE, " { \"target\":38.0 } \n").target == 38);
  CHECK(accept(Operation::TEMPERATURE, "{\"target\":3.8e1}").target == 38);
  CHECK(accept(Operation::TEMPERATURE, "{\"tar\\u0067et\":38}").target == 38);
  for (const auto *body : {"{}", "{\"target\":34}", "{\"target\":51}", "{\"target\":38.1}",
      "{\"target\":true}", "{\"target\":false}", "{\"target\":\"38\"}", "{\"target\":null}",
      "{\"target\":[]}", "{\"target\":{}}", "{\"target\":999999999999999999999}", "{\"target\":1e309}",
      "{\"target\":38,\"extra\":1}", "{\"temperature\":38}"}) reject(Operation::TEMPERATURE, body);
  for (int slot = 1; slot <= 3; ++slot)
    CHECK(accept(Operation::FAVORITE, "{\"slot\":" + std::to_string(slot) + "}").slot == slot);
  CHECK(accept(Operation::FAVORITE, "{\"slot\":2.0}").slot == 2);
  for (const auto *body : {"{}", "{\"slot\":0}", "{\"slot\":4}", "{\"slot\":1.1}",
      "{\"slot\":true}", "{\"slot\":\"1\"}", "{\"slot\":1,\"target\":38}"}) reject(Operation::FAVORITE, body);
}

static void tariff_settings() {
  auto result = accept(Operation::SETTINGS, "{\"gas_type\":\"GN\",\"gas_tariff\":0,\"water_tariff\":100000}");
  CHECK(!result.glp && result.gas_tariff == 0 && result.water_tariff == 100000);
  result = accept(Operation::SETTINGS, "{\"gas_type\":\"GLP\",\"gas_tariff\":12.34,\"water_tariff\":1e2}");
  CHECK(result.glp && std::fabs(result.gas_tariff - 12.34) < 1e-5 && result.water_tariff == 100);
  for (const auto *body : {"{}", "{\"gas_type\":\"GN\",\"gas_tariff\":0}",
      "{\"gas_type\":\"gn\",\"gas_tariff\":0,\"water_tariff\":0}",
      "{\"gas_type\":\"GNx\",\"gas_tariff\":0,\"water_tariff\":0}",
      "{\"gas_type\":false,\"gas_tariff\":0,\"water_tariff\":0}",
      "{\"gas_type\":\"GN\",\"gas_tariff\":-0.01,\"water_tariff\":0}",
      "{\"gas_type\":\"GN\",\"gas_tariff\":100000.01,\"water_tariff\":0}",
      "{\"gas_type\":\"GN\",\"gas_tariff\":false,\"water_tariff\":0}",
      "{\"gas_type\":\"GN\",\"gas_tariff\":\"1\",\"water_tariff\":0}",
      "{\"gas_type\":\"GN\",\"gas_tariff\":null,\"water_tariff\":0}",
      "{\"gas_type\":\"GN\",\"gas_tariff\":0,\"water_tariff\":1e999}",
      "{\"gas_type\":\"GN\",\"gas_tariff\":0,\"water_tariff\":0,\"favorites\":[]}"}) reject(Operation::SETTINGS, body);
}

static void favorites_preferences() {
  auto result = accept(Operation::PREFERENCES, PREFERENCES);
  CHECK(heater_health::valid_settings(result.preferences));
  CHECK(result.preferences.enabled_mask == heater_health::ALL_ALERTS);
  CHECK(std::string(result.preferences.favorites[0].name) == "Banho");
  auto fixture = preferences();
  fixture["favorites"][0]["name"] = "  Meu banho  ";
  fixture["alerts"]["flow_enabled"] = false;
  fixture["alerts"]["long_use_min"] = 1;
  fixture["alerts"]["continuous_flow_min"] = 180;
  fixture["alerts"]["offline_seconds"] = 600;
  result = accept(Operation::PREFERENCES, json(fixture));
  CHECK(std::string(result.preferences.favorites[0].name) == "Meu banho");
  CHECK((result.preferences.enabled_mask & heater_health::CONTINUOUS_FLOW) == 0);
  CHECK(result.preferences.long_use_min == 1 && result.preferences.continuous_flow_min == 180 && result.preferences.offline_seconds == 600);
  fixture["favorites"][0]["name"] = std::string(29, 'a') + "\xC3\xA9";
  result = accept(Operation::PREFERENCES, json(fixture));
  CHECK(std::strlen(result.preferences.favorites[0].name) == 31);
  fixture["favorites"][0]["name"] = std::string(30, 'a') + "\xC3\xA9";
  reject(Operation::PREFERENCES, json(fixture));
  for (const std::string &name : {std::string{}, std::string("   "), std::string("bad\nname"),
      std::string("bad\tname"), std::string("bad\x7F"), std::string("\xC0\xAF"), std::string("\xED\xA0\x80")}) {
    fixture = preferences(); fixture["favorites"][0]["name"] = name;
    reject(Operation::PREFERENCES, json(fixture));
  }
  for (int count : {0, 1, 2, 4}) {
    fixture = preferences();
    const auto favorites = fixture["favorites"].to<JsonArray>();
    for (int i = 0; i < count; ++i) { const auto row = favorites.add<JsonObject>(); row["name"] = "Banho"; row["temp_c"] = 38; }
    reject(Operation::PREFERENCES, json(fixture));
  }
  fixture = preferences(); fixture["favorites"][0] = "Banho";
  reject(Operation::PREFERENCES, json(fixture));
  fixture = preferences(); fixture["favorites"][0]["extra"] = 1;
  reject(Operation::PREFERENCES, json(fixture));
  for (const auto *key : {"name", "temp_c"}) {
    fixture = preferences(); fixture["favorites"][0].as<JsonObject>().remove(key);
    reject(Operation::PREFERENCES, json(fixture));
  }
  for (const auto *name : {"long_use_enabled", "flow_enabled", "error_enabled", "offline_enabled"}) {
    fixture = preferences(); fixture["alerts"][name] = 1;
    reject(Operation::PREFERENCES, json(fixture));
    fixture = preferences(); fixture["alerts"].as<JsonObject>().remove(name);
    reject(Operation::PREFERENCES, json(fixture));
  }
  for (const auto *name : {"long_use_min", "continuous_flow_min"}) {
    for (int value : {0, 181}) {
      fixture = preferences(); fixture["alerts"][name] = value;
      reject(Operation::PREFERENCES, json(fixture));
    }
  }
  for (int value : {9, 601}) {
    fixture = preferences(); fixture["alerts"]["offline_seconds"] = value;
    reject(Operation::PREFERENCES, json(fixture));
  }
  fixture = preferences(); fixture["alerts"]["long_use_min"] = true;
  reject(Operation::PREFERENCES, json(fixture));
  fixture = preferences(); fixture["alerts"]["offline_seconds"] = "30";
  reject(Operation::PREFERENCES, json(fixture));
  fixture = preferences(); fixture["favorites"][0]["temp_c"] = 38.5;
  reject(Operation::PREFERENCES, json(fixture));
  fixture = preferences(); fixture["favorites"][0]["temp_c"] = 38.0;
  CHECK(accept(Operation::PREFERENCES, json(fixture)).preferences.favorites[0].temperature_c == 38);
  fixture = preferences(); fixture["alerts"]["extra"] = 0;
  reject(Operation::PREFERENCES, json(fixture));
  fixture = preferences(); fixture["unexpected"] = 0;
  reject(Operation::PREFERENCES, json(fixture));
}

static void grammar_and_confirmations() {
  accept(Operation::CANCEL, "{}");
  CHECK(accept(Operation::RESTART, "{\"confirm\":true}").confirm);
  for (const auto *body : {"{}", "{\"confirm\":false}", "{\"confirm\":1}", "{\"confirm\":\"true\"}",
      "{\"confirm\":true,\"extra\":1}"}) reject(Operation::RESTART, body);
  reject(Operation::CANCEL, "{\"confirm\":true}");
  for (const auto *body : {"", " ", "null", "[]", "true", "1", "\"text\"", "{target:38}", "{'target':38}",
      "{\"target\":38,}", "{\"target\":038}", "{\"target\":+38}", "{\"target\":38.}",
      "{\"target\":.38}", "{\"target\":38e}", "{\"target\":NaN}", "{\"target\":Infinity}",
      "{\"target\":38} {}", "{\"target\":38}garbage", "{\"target\":38}//comment", "/*x*/{\"target\":38}",
      "{\"target\":38,\"target\":39}", "{\"target\":38,\"tar\\u0067et\":39}",
      "{\"target\":\"\\uD800\"}", "{\"target\":\"\\uDC00\"}", "{\"target\":\"\\uD800\\u0041\"}",
      "{\"target\":\"\\uZZZZ\"}", "{\"target\":\"\\q\"}", "{\"target\":\"\\u0000\"}"})
    reject(Operation::TEMPERATURE, body);
  std::string nul = "{\"target\":38}"; nul.push_back('\0');
  reject(Operation::TEMPERATURE, nul);
  reject(Operation::TEMPERATURE, "{\"target\":38}" + std::string(2048, ' '));
  const std::string minimum = "{\"target\":38}";
  CHECK(accept(Operation::TEMPERATURE, minimum + std::string(2048 - minimum.size(), ' ')).target == 38);
  for (size_t size = 1; size < minimum.size(); ++size) reject(Operation::TEMPERATURE, minimum.substr(0, size));
  reject(Operation::TEMPERATURE, "{\"target\":[[[[[[[38]]]]]]]}");
  auto unicode = std::string(PREFERENCES);
  unicode.replace(unicode.find("Banho"), 5, "Banho \\uD83D\\uDEBF");
  CHECK(std::string(accept(Operation::PREFERENCES, unicode).preferences.favorites[0].name) == "Banho \xF0\x9F\x9A\xBF");
  unicode = PREFERENCES;
  unicode.replace(unicode.find("Banho"), 5, "Banho\\u0000x");
  reject(Operation::PREFERENCES, unicode);
  reject(static_cast<Operation>(999), "{}");
}

int main() {
  targets_and_favorites();
  tariff_settings();
  favorites_preferences();
  grammar_and_confirmations();
  std::cout << "API request validation: " << checks << " checks passed\n";
}
