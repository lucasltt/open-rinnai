#pragma once

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart_component_esp_idf.h"

// Changes receiver settings only. Never assigns a TX pin or writes UART data.
namespace rinnai_rx {
inline bool configure(esphome::uart::IDFUARTComponent *first,
                      esphome::uart::IDFUARTComponent *second,
                      int baud, bool inverted, int parity, int stops) {
  const bool valid_baud = baud == 1200 || baud == 2400 || baud == 4800 || baud == 9600 ||
                          baud == 19200 || baud == 38400 || baud == 57600 || baud == 115200;
  if (!valid_baud || parity < 0 || parity > 2 || (stops != 1 && stops != 2)) {
    ESP_LOGW("rinnai.rx", "Unsupported receiver configuration");
    return false;
  }
  const uart_parity_t parity_mode = parity == 1 ? UART_PARITY_EVEN :
                                    parity == 2 ? UART_PARITY_ODD : UART_PARITY_DISABLE;
  for (auto *bus : {first, second}) {
    const auto port = static_cast<uart_port_t>(bus->get_hw_serial_number());
    // UART0 belongs exclusively to the USB bridge.
    if (port != UART_NUM_1 && port != UART_NUM_2) return false;
    esp_err_t result = uart_set_baudrate(port, baud);
    if (result == ESP_OK) result = uart_set_parity(port, parity_mode);
    if (result == ESP_OK) result = uart_set_stop_bits(port, stops == 1 ? UART_STOP_BITS_1 : UART_STOP_BITS_2);
    if (result == ESP_OK) result = uart_set_line_inverse(port, inverted ? UART_SIGNAL_RXD_INV : 0);
    if (result != ESP_OK) {
      ESP_LOGE("rinnai.rx", "UART%u configuration failed: %s", port, esp_err_to_name(result));
      return false;
    }
    // Only the decoder changes. GPIO input routing, levels, pulls and ISR stay intact.
    bus->set_baud_rate(baud);
    bus->set_parity(static_cast<esphome::uart::UARTParityOptions>(parity));
    bus->set_stop_bits(stops);
  }
  // Changing RX polarity can manufacture a partial character/BREAK inside the
  // receiver even with a static physical pad. Let that settle, then discard the
  // diagnostic transition bytes before ESPHome's RX debugger reads them.
  // The main loop is suspended briefly; UART interrupts continue servicing RX.
  vTaskDelay(pdMS_TO_TICKS(50));
  for (auto *bus : {first, second}) {
    const auto port = static_cast<uart_port_t>(bus->get_hw_serial_number());
    if (uart_flush_input(port) != ESP_OK) return false;
  }
  ESP_LOGI("rinnai.rx", "RX only: %d baud 8%c%d, inverted=%s", baud,
           parity == 1 ? 'E' : parity == 2 ? 'O' : 'N', stops, inverted ? "yes" : "no");
  return true;
}
}  // namespace rinnai_rx
