#pragma once

// Passive diagnostics for the two existing ESP32 UART RX pads.
// Call begin() AFTER ESPHome has set up its UARTs (e.g. on_boot priority -100).
// This helper owns only the GPIO edge interrupts on GPIO25/26. Do not combine
// it with another GPIO interrupt consumer on those pads.
//
// No gpio_config(), gpio_set_direction(), pull configuration, output operation,
// UART driver installation, or GPIO-matrix routing is performed here.
// IDF 5.5 pcnt_new_channel() enables a pull-up on a physical input, so PCNT is
// deliberately not used. GPIO ANYEDGE counts are diagnostic, not a logic capture:
// interrupt latency/coalescing, fast edges and cache-off intervals can lose edges.
// A zero count therefore does not prove that the physical line had no activity.

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "soc/gpio_struct.h"

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "line_diagnostics.h is limited to the classic ESP32 GPIO25/GPIO26 build"
#endif

namespace rinnai_diag {

namespace detail {

struct Line {
  explicit constexpr Line(gpio_num_t value) : pin(value) {}
  gpio_num_t pin;
  volatile uint32_t edge_count{0};
  uint32_t sampled_edges{0};
  int sampled_level{-1};
  esp_err_t last_error{ESP_ERR_INVALID_STATE};
  gpio_io_config_t initial_io{};
  gpio_int_type_t initial_interrupt_type{GPIO_INTR_DISABLE};
  bool installed{false};
  bool io_preserved{false};
  bool reclaimed_stale_irq{false};
};

// Static storage remains in internal DRAM on this ESP32-WROOM build. The ISR
// never allocates, logs, reads UARTs, or invokes application callbacks.
inline Line lines[2]{Line(GPIO_NUM_25), Line(GPIO_NUM_26)};
inline bool attempted{false};
inline bool reused_service{false};
inline bool service_available{false};
inline char status_text[240]{"not_started"};

static void IRAM_ATTR on_edge(void *context) {
  auto *line = static_cast<Line *>(context);
  line->edge_count = line->edge_count + uint32_t{1};
}

inline bool same_io(const gpio_io_config_t &a, const gpio_io_config_t &b) {
  return a.fun_sel == b.fun_sel && a.sig_out == b.sig_out && a.drv == b.drv && a.pu == b.pu &&
         a.pd == b.pd && a.ie == b.ie && a.oe == b.oe && a.oe_ctrl_by_periph == b.oe_ctrl_by_periph &&
         a.oe_inv == b.oe_inv && a.od == b.od && a.slp_sel == b.slp_sel;
}

inline bool line_ready(const Line &line) {
  return line.installed && line.io_preserved && line.last_error == ESP_OK;
}

inline void refresh_status() {
  std::snprintf(status_text, sizeof(status_text),
                "P1:%s io=%s [ie=%u oe=%u irq0=%u rec=%u]; P2:%s io=%s [ie=%u oe=%u irq0=%u rec=%u]; ISR=%s",
                esp_err_to_name(lines[0].last_error), lines[0].io_preserved ? "kept" : "unverified",
                static_cast<unsigned>(lines[0].initial_io.ie), static_cast<unsigned>(lines[0].initial_io.oe),
                static_cast<unsigned>(lines[0].initial_interrupt_type),
                static_cast<unsigned>(lines[0].reclaimed_stale_irq),
                esp_err_to_name(lines[1].last_error), lines[1].io_preserved ? "kept" : "unverified",
                static_cast<unsigned>(lines[1].initial_io.ie), static_cast<unsigned>(lines[1].initial_io.oe),
                static_cast<unsigned>(lines[1].initial_interrupt_type),
                static_cast<unsigned>(lines[1].reclaimed_stale_irq),
                reused_service ? "existing_blocked" : (service_available ? "new_IRAM" : "unavailable"));
}

}  // namespace detail

// index 0 = P1/GPIO25; index 1 = P2/GPIO26.
// These are pad levels, independent of UART line inversion and baud settings.
inline int level(size_t index) {
  return index < 2 ? gpio_get_level(detail::lines[index].pin) : -1;
}

// Monotonic modulo 2^32, since successful initialization. One aligned 32-bit load
// reads the single-ISR-writer counter on this target. No counter is cleared while
// the UART is running; a delta can be calculated as uint32_t(new_count-old_count).
inline uint32_t edges(size_t index) {
  return index < 2 ? detail::lines[index].edge_count : 0;
}

inline bool ready(size_t index) {
  return index < 2 && detail::line_ready(detail::lines[index]);
}

inline bool ready() { return ready(0) && ready(1); }

inline esp_err_t error(size_t index) {
  return index < 2 ? detail::lines[index].last_error : ESP_ERR_INVALID_ARG;
}

inline const char *status() { return detail::status_text; }

// Refreshes a snapshot and verifies that mode/pulls/output mapping still match
// their pre-initialization values. It performs only reads of the GPIO state.
// level() and edges() above are live reads and do not require this call first.
inline void update() {
  if (!detail::attempted)
    return;
  for (auto &line : detail::lines) {
    line.sampled_level = gpio_get_level(line.pin);
    line.sampled_edges = line.edge_count;
    if (!line.installed)
      continue;
    gpio_io_config_t current{};
    const esp_err_t err = gpio_get_io_config(line.pin, &current);
    line.io_preserved = err == ESP_OK && detail::same_io(line.initial_io, current);
    if (!line.io_preserved)
      line.last_error = err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
  }
  detail::refresh_status();
}

// Idempotent: a second call never installs duplicate handlers or resets counts.
// Rejects pads that are not already input-only. A fresh ISR service is required:
// IDF initializes its handler table with calloc, proving that no previous GPIO
// service consumers exist in this boot. The public API cannot inspect handlers
// in an existing service, so that case is rejected without replacing anything.
// After a soft/OTA reset the hardware int_type can survive although the handler
// table does not; a fresh service can safely reclaim that residual register.
inline bool begin() {
  if (detail::attempted) {
    update();
    return ready();
  }
  detail::attempted = true;

  bool any_candidate = false;
  for (auto &line : detail::lines) {
    line.last_error = gpio_get_io_config(line.pin, &line.initial_io);
    line.initial_interrupt_type = static_cast<gpio_int_type_t>(GPIO.pin[line.pin].int_type);
    if (line.last_error == ESP_OK && (!line.initial_io.ie || line.initial_io.oe)) {
      line.last_error = ESP_ERR_INVALID_STATE;
    }
    any_candidate = any_candidate || line.last_error == ESP_OK;
  }
  if (!any_candidate) {
    update();
    return false;
  }

  const esp_err_t service_err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  detail::reused_service = service_err == ESP_ERR_INVALID_STATE;
  detail::service_available = service_err == ESP_OK;
  if (service_err != ESP_OK) {
    for (auto &line : detail::lines) {
      if (line.last_error == ESP_OK)
        line.last_error = service_err;
    }
    update();
    return false;
  }

  for (auto &line : detail::lines) {
    if (line.last_error != ESP_OK)
      continue;
    line.reclaimed_stale_irq = line.initial_interrupt_type != GPIO_INTR_DISABLE;
    line.last_error = gpio_set_intr_type(line.pin, GPIO_INTR_ANYEDGE);
    if (line.last_error == ESP_OK)
      line.last_error = gpio_isr_handler_add(line.pin, detail::on_edge, &line);
    if (line.last_error == ESP_OK) {
      line.installed = true;
    } else {
      // The fresh service has no previous consumers, so rollback to disabled
      // cannot remove another handler and does not modify any pad setting.
      gpio_intr_disable(line.pin);
      gpio_set_intr_type(line.pin, GPIO_INTR_DISABLE);
    }
  }
  update();
  return ready();
}

}  // namespace rinnai_diag
