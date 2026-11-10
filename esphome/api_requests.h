#pragma once

// Validation only: no networking, persistence, controller access, or I/O.
#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>
#include "heater_health.h"

namespace rinnai_api_requests {

inline constexpr size_t MAX_BODY_BYTES = 2048;
inline constexpr unsigned MAX_NESTING = 6;

enum class Operation { TEMPERATURE, FAVORITE, SETTINGS, PREFERENCES, CANCEL, RESTART };

struct WriteRequest {
  int target{0}, slot{0};
  bool confirm{false};
  heater_health::Settings preferences{};
  bool glp{false};
  double gas_tariff{0}, water_tariff{0};
};

namespace detail {

// ArduinoJson deliberately accepts some non-JSON syntax. Enforce JSON grammar
// and unique decoded object keys before handing this bounded body to it.
class StrictJson {
 public:
  explicit StrictJson(const std::string &input) : input_(input) {}
  bool valid() { whitespace_(); return value_(0) && (whitespace_(), offset_ == input_.size()); }

 private:
  bool take_(char value) {
    if (offset_ >= input_.size() || input_[offset_] != value) return false;
    ++offset_; return true;
  }
  void whitespace_() {
    while (offset_ < input_.size() && (input_[offset_] == ' ' || input_[offset_] == '\t' ||
           input_[offset_] == '\r' || input_[offset_] == '\n')) ++offset_;
  }
  bool literal_(const char *value) {
    const size_t length = std::strlen(value);
    if (input_.compare(offset_, length, value) != 0) return false;
    offset_ += length; return true;
  }
  static int hex_(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  }
  bool codepoint_(uint32_t &value) {
    if (input_.size() - offset_ < 4) return false;
    value = 0;
    for (unsigned i = 0; i < 4; ++i) {
      const int digit = hex_(input_[offset_++]);
      if (digit < 0) return false;
      value = (value << 4) | static_cast<uint32_t>(digit);
    }
    return true;
  }
  static void utf8_(std::string &out, uint32_t value) {
    if (value < 0x80) out += static_cast<char>(value);
    else if (value < 0x800) {
      out += static_cast<char>(0xC0 | (value >> 6)); out += static_cast<char>(0x80 | (value & 63));
    } else if (value < 0x10000) {
      out += static_cast<char>(0xE0 | (value >> 12)); out += static_cast<char>(0x80 | ((value >> 6) & 63));
      out += static_cast<char>(0x80 | (value & 63));
    } else {
      out += static_cast<char>(0xF0 | (value >> 18)); out += static_cast<char>(0x80 | ((value >> 12) & 63));
      out += static_cast<char>(0x80 | ((value >> 6) & 63)); out += static_cast<char>(0x80 | (value & 63));
    }
  }
  bool string_(std::string *decoded = nullptr) {
    if (!take_('"')) return false;
    while (offset_ < input_.size()) {
      const auto value = static_cast<unsigned char>(input_[offset_++]);
      if (value == '"') return true;
      if (value < 0x20) return false;
      if (value != '\\') { if (decoded) *decoded += static_cast<char>(value); continue; }
      if (offset_ == input_.size()) return false;
      const char escape = input_[offset_++];
      char replacement = 0;
      switch (escape) {
        case '"': case '\\': case '/': replacement = escape; break;
        case 'b': replacement = '\b'; break;
        case 'f': replacement = '\f'; break;
        case 'n': replacement = '\n'; break;
        case 'r': replacement = '\r'; break;
        case 't': replacement = '\t'; break;
        case 'u': {
          uint32_t point = 0;
          if (!codepoint_(point) || point == 0) return false;
          if (point >= 0xD800 && point <= 0xDBFF) {
            uint32_t low = 0;
            if (!take_('\\') || !take_('u') || !codepoint_(low) || low < 0xDC00 || low > 0xDFFF) return false;
            point = 0x10000 + ((point - 0xD800) << 10) + low - 0xDC00;
          } else if (point >= 0xDC00 && point <= 0xDFFF) return false;
          if (decoded) utf8_(*decoded, point);
          continue;
        }
        default: return false;
      }
      if (decoded) *decoded += replacement;
    }
    return false;
  }
  static bool digit_(char value) { return value >= '0' && value <= '9'; }
  bool number_() {
    take_('-');
    if (offset_ == input_.size()) return false;
    if (input_[offset_] == '0') ++offset_;
    else {
      if (input_[offset_] < '1' || input_[offset_] > '9') return false;
      while (offset_ < input_.size() && digit_(input_[offset_])) ++offset_;
    }
    if (take_('.')) {
      if (offset_ == input_.size() || !digit_(input_[offset_])) return false;
      while (offset_ < input_.size() && digit_(input_[offset_])) ++offset_;
    }
    if (offset_ < input_.size() && (input_[offset_] == 'e' || input_[offset_] == 'E')) {
      ++offset_;
      if (offset_ < input_.size() && (input_[offset_] == '+' || input_[offset_] == '-')) ++offset_;
      if (offset_ == input_.size() || !digit_(input_[offset_])) return false;
      while (offset_ < input_.size() && digit_(input_[offset_])) ++offset_;
    }
    return true;
  }
  bool value_(unsigned depth) {
    whitespace_();
    if (offset_ == input_.size()) return false;
    const char value = input_[offset_];
    if (value == '"') return string_();
    if (value == '{' || value == '[') {
      if (depth >= MAX_NESTING) return false;
      const bool object = value == '{';
      ++offset_; whitespace_();
      const char closing = object ? '}' : ']';
      if (take_(closing)) return true;
      std::vector<std::string> keys;
      while (true) {
        if (object) {
          std::string key;
          if (!string_(&key) || std::find(keys.begin(), keys.end(), key) != keys.end()) return false;
          keys.push_back(std::move(key)); whitespace_();
          if (!take_(':')) return false;
        }
        if (!value_(depth + 1)) return false;
        whitespace_();
        if (take_(closing)) return true;
        if (!take_(',')) return false;
        whitespace_();
      }
    }
    if (value == 't') return literal_("true");
    if (value == 'f') return literal_("false");
    if (value == 'n') return literal_("null");
    return number_();
  }
  const std::string &input_;
  size_t offset_{0};
};

inline bool keys(JsonObjectConst object, std::initializer_list<const char *> required) {
  if (object.size() != required.size()) return false;
  for (const char *key : required) if (object[key].isUnbound()) return false;
  return true;
}

inline bool integer(JsonVariantConst value, int minimum, int maximum, int &out) {
  if (value.is<bool>() || !value.is<double>()) return false;
  const double result = value.as<double>();
  if (!std::isfinite(result) || result != std::floor(result) || result < minimum || result > maximum) return false;
  out = static_cast<int>(result); return true;
}

inline bool tariff(JsonVariantConst value, double &out) {
  if (value.is<bool>() || !value.is<double>()) return false;
  const double result = value.as<double>();
  if (!std::isfinite(result) || result < 0 || result > 100000) return false;
  out = result; return true;
}

inline bool preferences(JsonObjectConst root, heater_health::Settings &out) {
  if (!keys(root, {"favorites", "alerts"}) || !root["favorites"].is<JsonArrayConst>() ||
      !root["alerts"].is<JsonObjectConst>()) return false;
  const auto favorites = root["favorites"].as<JsonArrayConst>();
  if (favorites.size() != out.favorites.size()) return false;
  for (size_t i = 0; i < out.favorites.size(); ++i) {
    if (!favorites[i].is<JsonObjectConst>()) return false;
    const auto source = favorites[i].as<JsonObjectConst>();
    int temperature = 0;
    if (!keys(source, {"name", "temp_c"}) || !source["name"].is<const char *>() ||
        !integer(source["temp_c"], 35, 50, temperature)) return false;
    const JsonString raw = source["name"].as<JsonString>();
    std::string name(raw.c_str(), raw.size());
    const auto first = name.find_first_not_of(' '), last = name.find_last_not_of(' ');
    if (first == std::string::npos) return false;
    name = name.substr(first, last - first + 1);
    if (name.empty() || name.size() > 31 || name.find('\0') != std::string::npos) return false;
    auto &favorite = out.favorites[i];
    std::memset(favorite.name, 0, sizeof(favorite.name));
    std::memcpy(favorite.name, name.data(), name.size());
    favorite.temperature_c = temperature;
  }
  const auto alerts = root["alerts"].as<JsonObjectConst>();
  if (!keys(alerts, {"long_use_enabled", "flow_enabled", "error_enabled", "offline_enabled",
                    "long_use_min", "continuous_flow_min", "offline_seconds"})) return false;
  out.enabled_mask = 0;
  const char *flags[]{"long_use_enabled", "flow_enabled", "error_enabled", "offline_enabled"};
  const uint32_t masks[]{heater_health::LONG_USE, heater_health::CONTINUOUS_FLOW,
                         heater_health::HEATER_ERROR, heater_health::OFFLINE};
  for (size_t i = 0; i < 4; ++i) {
    if (!alerts[flags[i]].is<bool>()) return false;
    if (alerts[flags[i]].as<bool>()) out.enabled_mask |= masks[i];
  }
  int long_use = 0, flow = 0, offline = 0;
  if (!integer(alerts["long_use_min"], 1, 180, long_use) ||
      !integer(alerts["continuous_flow_min"], 1, 180, flow) ||
      !integer(alerts["offline_seconds"], 10, 600, offline)) return false;
  out.long_use_min = static_cast<uint32_t>(long_use);
  out.continuous_flow_min = static_cast<uint32_t>(flow);
  out.offline_seconds = static_cast<uint32_t>(offline);
  return heater_health::valid_settings(out);
}
}  // namespace detail

inline bool decode(Operation operation, const std::string &body, WriteRequest &out, std::string &error) {
  error.clear();
  if (body.empty() || body.size() > MAX_BODY_BYTES || body.find('\0') != std::string::npos) {
    error = "Envie um objeto JSON de ate 2048 bytes, sem NUL."; return false;
  }
  if (!detail::StrictJson(body).valid()) { error = "JSON invalido ou chaves duplicadas."; return false; }
  JsonDocument document;
  const auto result = deserializeJson(document, body, DeserializationOption::NestingLimit(MAX_NESTING));
  if (result || !document.is<JsonObjectConst>()) { error = "Envie um objeto JSON valido."; return false; }
  const auto root = document.as<JsonObjectConst>();
  WriteRequest parsed;
  bool valid = false;
  switch (operation) {
    case Operation::TEMPERATURE:
      valid = detail::keys(root, {"target"}) && detail::integer(root["target"], 35, 50, parsed.target);
      if (!valid) error = "target deve ser um inteiro entre 35 e 50.";
      break;
    case Operation::FAVORITE:
      valid = detail::keys(root, {"slot"}) && detail::integer(root["slot"], 1, 3, parsed.slot);
      if (!valid) error = "slot deve ser um inteiro entre 1 e 3.";
      break;
    case Operation::SETTINGS:
      valid = detail::keys(root, {"gas_type", "gas_tariff", "water_tariff"}) && root["gas_type"].is<const char *>() &&
              detail::tariff(root["gas_tariff"], parsed.gas_tariff) && detail::tariff(root["water_tariff"], parsed.water_tariff);
      if (valid) {
        const JsonString kind = root["gas_type"].as<JsonString>();
        valid = kind.size() == 2 && std::memcmp(kind.c_str(), "GN", 2) == 0;
        parsed.glp = kind.size() == 3 && std::memcmp(kind.c_str(), "GLP", 3) == 0;
        valid = valid || parsed.glp;
      }
      if (!valid) error = "Envie gas_type GN ou GLP e tarifas numericas entre 0 e 100000.";
      break;
    case Operation::PREFERENCES:
      valid = detail::preferences(root, parsed.preferences);
      if (!valid) error = "Confira os tres favoritos, os avisos e os limites.";
      break;
    case Operation::CANCEL:
      valid = root.size() == 0;
      if (!valid) error = "O cancelamento aceita somente um objeto vazio.";
      break;
    case Operation::RESTART:
      valid = detail::keys(root, {"confirm"}) && root["confirm"].is<bool>() && root["confirm"].as<bool>();
      parsed.confirm = valid;
      if (!valid) error = "Confirme o reinicio do modulo com confirm: true.";
      break;
    default: error = "Operacao desconhecida."; break;
  }
  if (!valid) return false;
  out = parsed;
  return true;
}
}  // namespace rinnai_api_requests
