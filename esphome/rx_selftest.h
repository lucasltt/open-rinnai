#pragma once

// ESP32-classic RX-only self-test. No external pin is driven: the UART input
// matrix is temporarily routed to internal constant levels, never to a TX pad.
// Call synchronously from the ESPHome main-loop context, while UART debugger
// consumers are not running. Synthetic bytes are consumed/flushed here before
// physical RX is restored, so they must never reach heater byte counters.
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include "sdkconfig.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esphome/core/log.h"

#if defined(CONFIG_IDF_TARGET_ESP32)
#include "soc/gpio_pins.h"
#include "soc/gpio_sig_map.h"
#endif

namespace rinnai_rx_selftest {

#if defined(CONFIG_IDF_TARGET_ESP32)
namespace detail {

inline bool normalize(uart_port_t port) {
  // Evaluate every setter, including during cleanup; do not short-circuit.
  bool ok = true;
  ok = (uart_set_baudrate(port, 9600) == ESP_OK) && ok;
  ok = (uart_set_word_length(port, UART_DATA_8_BITS) == ESP_OK) && ok;
  ok = (uart_set_parity(port, UART_PARITY_DISABLE) == ESP_OK) && ok;
  ok = (uart_set_stop_bits(port, UART_STOP_BITS_1) == ESP_OK) && ok;
  ok = (uart_set_line_inverse(port, UART_SIGNAL_INV_DISABLE) == ESP_OK) && ok;
  return ok;
}

class RxRouteGuard {
 public:
  RxRouteGuard(uart_port_t port, int gpio, uint32_t signal)
      : port_(port), gpio_(gpio), signal_(signal) {
    // GPIO_MATRIX_CONST_* are internal matrix sources, not physical GPIOs.
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, signal_, false);
  }

  bool restore() {
    if (restored_)
      return restore_ok_;
    // Hold internal idle HIGH while normalizing and discarding all synthetic
    // characters. Only then reconnect the original physical RX input.
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, signal_, false);
    restore_ok_ = normalize(port_);
    esp_rom_delay_us(2500);
    restore_ok_ = (uart_flush_input(port_) == ESP_OK) && restore_ok_;
    esp_rom_gpio_connect_in_signal(static_cast<uint32_t>(gpio_), signal_, false);
    restored_ = true;
    return restore_ok_;
  }

  ~RxRouteGuard() { restore(); }
  RxRouteGuard(const RxRouteGuard &) = delete;
  RxRouteGuard &operator=(const RxRouteGuard &) = delete;

 private:
  uart_port_t port_;
  int gpio_;
  uint32_t signal_;
  bool restored_{false};
  bool restore_ok_{false};
};

}  // namespace detail
#endif

inline std::string run(uart_port_t port, int rx_gpio) {
#if !defined(CONFIG_IDF_TARGET_ESP32)
  return "SKIP: internal RX test supports ESP32 classic only";
#else
  // Match this project's fixed assignment, and exclude the USB UART0.
  if (!((port == UART_NUM_1 && rx_gpio == 25) ||
        (port == UART_NUM_2 && rx_gpio == 26)))
    return "FAIL: unexpected UART/GPIO pair; no route changed";
  if (!uart_is_driver_installed(port))
    return "FAIL: UART driver not installed; no route changed";

  const uint32_t signal = port == UART_NUM_1 ? U1RXD_IN_IDX : U2RXD_IN_IDX;
  detail::RxRouteGuard route(port, rx_gpio, signal);
  if (!detail::normalize(port)) {
    const bool restored = route.restore();
    return restored ? "FAIL: UART configuration; physical RX restored"
                    : "FAIL: UART configuration/restore; physical RX reconnected";
  }

  // Let an old partial character/BREAK finish on the now-internal idle line.
  esp_rom_delay_us(2500);
  if (uart_flush_input(port) != ESP_OK) {
    route.restore();
    return "FAIL: cannot flush RX; physical RX reconnected";
  }

  constexpr std::array<uint8_t, 6> expected{{0x55, 0xAA, 0x00, 0xFF, 0x12, 0xE7}};
  std::array<uint8_t, expected.size()> received{};

  // 104 us is within 0.2% of a 9600-baud bit. The ROM calls add small
  // overhead. Briefly mask interrupts on this core for deterministic timing;
  // six bytes fit easily in the ESP32 UART hardware FIFO. No peripheral TX,
  // gpio_set_level, pin direction, pulls, or external output routing is used.
  portMUX_TYPE timing_lock = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&timing_lock);
  for (uint8_t value : expected) {
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, signal, false);
    esp_rom_delay_us(104);  // start bit
    for (unsigned bit = 0; bit < 8; ++bit) {
      const uint32_t source = (value & (1U << bit))
                                  ? GPIO_MATRIX_CONST_ONE_INPUT
                                  : GPIO_MATRIX_CONST_ZERO_INPUT;
      esp_rom_gpio_connect_in_signal(source, signal, false);
      esp_rom_delay_us(104);
    }
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, signal, false);
    esp_rom_delay_us(104);  // one stop bit
  }
  portEXIT_CRITICAL(&timing_lock);

  // ISR/ring-buffer processing resumes here, with the input still internal.
  const int count = uart_read_bytes(port, received.data(), received.size(), pdMS_TO_TICKS(60));
  const bool matched = count == static_cast<int>(expected.size()) &&
                       std::memcmp(expected.data(), received.data(), expected.size()) == 0;
  const bool restored = route.restore();
  char result[140];
  std::snprintf(result, sizeof(result),
                "%s UART%d internal RX (%d/6 bytes, pattern %s); GPIO%d %s, 9600 8N1 normal",
                matched && restored ? "PASS" : "FAIL", static_cast<int>(port), count,
                matched ? "matched" : "mismatch", rx_gpio,
                restored ? "restored" : "reconnected/config error");
  ESP_LOGI("rinnai.selftest", "%s; synthetic bytes discarded", result);
  return result;
#endif
}

}  // namespace rinnai_rx_selftest
