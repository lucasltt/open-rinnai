#pragma once

#include <algorithm>
#include <cstring>
#include <esp_app_desc.h>
#include <esp_system.h>
#include "esphome/components/logger/logger.h"
#include "esphome/components/esp32/crash_handler.h"
#include "heater_storage.h"

namespace rinnai_diagnostics {

inline const char *reset_reason_text(uint32_t reason) {
  switch (reason) {
    case 0: return "Motivo desconhecido";
    case 1: return "Energia ligada";
    case 2: return "Reset externo";
    case 3: return "Reinicio solicitado";
    case 4: return "Excecao do firmware";
    case 5: return "Watchdog de interrupcao";
    case 6: return "Watchdog de tarefa";
    case 7: return "Watchdog";
    case 8: return "Saida de suspensao";
    case 9: return "Tensao insuficiente";
    case 10: return "Reset por SDIO";
    case 11: return "Reset por USB";
    case 12: return "Reset por depuracao";
    case 13: return "Erro de eFuse";
    case 14: return "Oscilacao de energia";
    case 15: return "Travamento da CPU";
    default: return "Motivo nao classificado";
  }
}

struct Identity {
  uint32_t version{1};
  char elf_sha256[65]{};
  char project_version[16]{};
  uint8_t reserved[3]{};
};
struct CrashPersist {
  uint32_t version{1}, length{0}, boot_sequence{0}, reason_code{0};
  uint64_t observed_at_epoch_s{0};
  char elf_sha256[65]{};
  char project_version[16]{};
  char text[2048]{};
  uint8_t reserved[7]{};
};
static_assert(sizeof(Identity) == 88, "Stable firmware identity layout");
static_assert(sizeof(CrashPersist) == 2160, "Stable crash log layout");
inline Identity current_identity{}, previous_identity{};
inline CrashPersist crash{}, capture_work{};
inline esphome::ESPPreferenceObject crash_preference{}, identity_preference{};
inline bool initialized{false}, capture_enabled{false}, storage_ok{true};

inline void receive_log(void *, uint8_t, const char *tag, const char *message, size_t length) {
  if (!capture_enabled || !tag || std::strcmp(tag, "esp32.crash") != 0) return;
  bool escape = false;
  for (size_t i = 0; i < length && capture_work.length + 1 < sizeof(capture_work.text); ++i) {
    const auto value = static_cast<unsigned char>(message[i]);
    if (value == 0x1B) { escape = true; continue; }
    if (escape) { if (value >= 0x40 && value <= 0x7E && value != '[') escape = false; continue; }
    if (value >= 0x20 || value == '\t') capture_work.text[capture_work.length++] = static_cast<char>(value);
  }
  if (capture_work.length + 1 < sizeof(capture_work.text)) capture_work.text[capture_work.length++] = '\n';
  capture_work.text[capture_work.length] = '\0';
}

inline void begin() {
  if (initialized || !esphome::global_preferences) return;
  crash_preference = esphome::global_preferences->make_preference<CrashPersist>(0x6EA91C58);
  identity_preference = esphome::global_preferences->make_preference<Identity>(0x6EA91C59);
  if (!crash_preference.load(&crash) || crash.version != 1 || crash.length >= sizeof(crash.text) ||
      crash.text[crash.length] != '\0' || crash.elf_sha256[64] != '\0' || crash.project_version[15] != '\0')
    crash = CrashPersist{};
  if (!identity_preference.load(&previous_identity) || previous_identity.version != 1 ||
      previous_identity.elf_sha256[64] != '\0' || previous_identity.project_version[15] != '\0')
    previous_identity = Identity{};
  // IDF's formatted helper can be configured to expose only a short prefix.
  // The app description retains all 32 digest bytes needed to identify an ELF.
  constexpr char hex[] = "0123456789abcdef";
  const auto *description = esp_app_get_description();
  for (size_t i = 0; i < 32; ++i) {
    current_identity.elf_sha256[2 * i] = hex[description->app_elf_sha256[i] >> 4];
    current_identity.elf_sha256[2 * i + 1] = hex[description->app_elf_sha256[i] & 0x0F];
  }
  current_identity.elf_sha256[64] = '\0';
  std::strncpy(current_identity.project_version, ESPHOME_PROJECT_VERSION,
               sizeof(current_identity.project_version) - 1);
#ifdef USE_ESP32_CRASH_HANDLER
  if (esphome::logger::global_logger && esphome::esp32::crash_handler_has_data()) {
    // logger.on_message reserves a generated listener slot. Registering here
    // would overflow ESPHome's fixed-capacity callback list.
    capture_work = CrashPersist{};
    capture_work.boot_sequence = rinnai_storage::health.boot_count();
    capture_work.reason_code = static_cast<uint32_t>(esp_reset_reason());
    capture_work.observed_at_epoch_s = rinnai_storage::epoch_now();
    std::memcpy(capture_work.elf_sha256, previous_identity.elf_sha256, sizeof(capture_work.elf_sha256));
    std::memcpy(capture_work.project_version, previous_identity.project_version, sizeof(capture_work.project_version));
    capture_enabled = true;
    esphome::esp32::crash_handler_log();
    capture_enabled = false;
    if (capture_work.length) {
      storage_ok = crash_preference.save(&capture_work) && esphome::global_preferences->sync();
      if (storage_ok) {
        crash = capture_work;
        esphome::esp32::crash_handler_clear();
      }
    }
  }
#endif
  if (storage_ok && std::memcmp(&previous_identity, &current_identity, sizeof(Identity)) != 0)
    storage_ok = identity_preference.save(&current_identity) && esphome::global_preferences->sync();
  initialized = true;
}
}  // namespace rinnai_diagnostics
