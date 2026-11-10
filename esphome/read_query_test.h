#pragma once

// One explicitly requested ROU003/1 transaction. Not called at boot and never
// retries. Only the fixed read whitelist and two fixed relative-temperature
// frames are available; there is no raw write, power, or toggle operation.
// Authorization, scheduling, observation guards and setpoint verification
// belong to the caller/controller. This header cannot determine voltage,
// pinout, or series-resistor wiring.
//
// Fixed project assignment: GPIO25 = UART1 RX, GPIO26 = UART2 RX. UART0/USB is
// never touched. Both RX drivers, GPIO ANYEDGE handlers, input matrix routes,
// input enable, and pulls remain active throughout the UART-driven transfer.
// The verified TX direction is GPIO25 -> heater -> GPIO26. Only GPIO25 may
// become a temporary open-drain TX output. A HIGH bit
// releases it; the ESP32 never actively drives the line HIGH. Hardware UART
// clocks a read's 120 bits in nominally 12,500 us at 9600 baud; each fixed
// relative-temperature command is 70 bits, nominally 7,292 us.
//
// Must run synchronously in ESPHome's main-loop context, with these UARTs owned
// by this RX-only project and no other producer of UART1/UART2 TX data. The
// caller owns cooldown/arming. A local UART echo is NOT a heater response and
// does not establish that a signal reached the far side of a series resistor.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "hal/uart_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/uart_periph.h"

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "read_query_test.h supports the classic ESP32 GPIO25/GPIO26 build only"
#endif

namespace rinnai_read_query {
namespace detail {

inline constexpr std::array<gpio_num_t, 2> PINS{{GPIO_NUM_25, GPIO_NUM_26}};
inline constexpr std::array<uart_port_t, 2> PORTS{{UART_NUM_1, UART_NUM_2}};
inline constexpr std::array<uint32_t, 2> RX_SIGNALS{{U1RXD_IN_IDX, U2RXD_IN_IDX}};
inline constexpr std::array<uint32_t, 2> TX_SIGNALS{{U1TXD_OUT_IDX, U2TXD_OUT_IDX}};

// No user-supplied opcode/data can reach the transmitter. Only these eight
// read addresses are accepted by run_register(); F6 and literal length "02"
// cannot be changed. Checksum is the uppercase ASCII hex sum of bytes 1..8.
inline constexpr std::array<uint16_t, 8> READ_REGISTERS{{
    0x0604, 0x0606, 0x0608, 0x0648, 0x060E, 0x0611, 0x0614, 0x060F}};

inline constexpr bool readable_register(uint16_t address) {
  for (const uint16_t allowed : READ_REGISTERS) {
    if (address == allowed)
      return true;
  }
  return false;
}

inline constexpr uint8_t hex_digit(unsigned value) {
  return static_cast<uint8_t>(value < 10 ? '0' + value : 'A' + value - 10);
}

// Internal frame construction only: the sending entry point performs the
// whitelist check before touching a GPIO, and has no raw-write overload.
inline constexpr std::array<uint8_t, 12> read_frame(uint16_t address) {
  std::array<uint8_t, 12> frame{{
      0x02, 0xF6, hex_digit((address >> 12) & 0x0F), hex_digit((address >> 8) & 0x0F),
      hex_digit((address >> 4) & 0x0F), hex_digit(address & 0x0F), 0x30, 0x32, 0x03,
      0x00, 0x00, 0x0D}};
  uint8_t sum = 0;
  for (size_t i = 1; i <= 8; ++i)
    sum = static_cast<uint8_t>(sum + frame[i]);
  frame[9] = hex_digit(sum >> 4);
  frame[10] = hex_digit(sum & 0x0F);
  return frame;
}

// Keep the two original literal frames as static reference vectors.
inline constexpr std::array<uint8_t, 12> OUTLET_0604{{
    0x02, 0xF6, 0x30, 0x36, 0x30, 0x34, 0x30, 0x32, 0x03, 0x32, 0x35, 0x0D}};
inline constexpr std::array<uint8_t, 12> INLET_0606{{
    0x02, 0xF6, 0x30, 0x36, 0x30, 0x36, 0x30, 0x32, 0x03, 0x32, 0x37, 0x0D}};

// Firmware-extracted relative setpoint operations. No arbitrary U-command,
// absolute write, toggle or power frame can be selected by the public API.
inline constexpr std::array<uint8_t, 7> INCREMENT{{0x02, 0x55, 0x30, 0x03, 0x38, 0x38, 0x0D}};
inline constexpr std::array<uint8_t, 7> DECREMENT{{0x02, 0x55, 0x31, 0x03, 0x38, 0x39, 0x0D}};
enum class Operation : uint8_t { READ, INCREMENT, DECREMENT };

inline std::atomic_flag busy = ATOMIC_FLAG_INIT;

class CallGuard {
 public:
  ~CallGuard() { busy.clear(std::memory_order_release); }
};

inline bool same_io(const gpio_io_config_t &a, const gpio_io_config_t &b) {
  return a.fun_sel == b.fun_sel && a.sig_out == b.sig_out && a.drv == b.drv &&
         a.pu == b.pu && a.pd == b.pd && a.ie == b.ie && a.oe == b.oe &&
         a.oe_ctrl_by_periph == b.oe_ctrl_by_periph && a.oe_inv == b.oe_inv &&
         a.od == b.od && a.slp_sel == b.slp_sel;
}

inline bool gpio_mux_alias(size_t index, uint32_t function) {
  // IDF 5.5.4's ESP32 io_mux_reg.h explicitly defines BOTH GPIO functions:
  // FUNC_GPIO25_GPIO25_0=0 / FUNC_GPIO25_GPIO25=2 (lines 305-306), and
  // FUNC_GPIO26_GPIO26_0=0 / FUNC_GPIO26_GPIO26=2 (lines 311-312).
  // RX-only uart_set_pin() can leave the reset alias 0 selected. Accept these
  // two documented GPIO aliases only, never Ethernet function 5 or other muxes.
  return function == PIN_FUNC_GPIO ||
         (index == 0 && function == FUNC_GPIO25_GPIO25_0) ||
         (index == 1 && function == FUNC_GPIO26_GPIO26_0);
}

struct Snapshot {
  std::array<gpio_io_config_t, 2> io{};
  std::array<uint32_t, 2> output_route{};
  std::array<uint32_t, 2> rx_route{};
  std::array<uint32_t, 2> irq_config{};
  uint32_t output_latch{0};
};

inline bool snapshot(Snapshot &state, char *reason, size_t capacity) {
  state.output_latch = GPIO.out;
  for (size_t i = 0; i < PINS.size(); ++i) {
    const auto pin = PINS[i];
    const esp_err_t err = gpio_get_io_config(pin, &state.io[i]);
    if (err != ESP_OK) {
      std::snprintf(reason, capacity, "GPIO%d config: %s", static_cast<int>(pin), esp_err_to_name(err));
      return false;
    }
    state.output_route[i] = GPIO.func_out_sel_cfg[pin].val;
    state.rx_route[i] = GPIO.func_in_sel_cfg[RX_SIGNALS[i]].val;
    state.irq_config[i] = GPIO.pin[pin].val;
    const auto &io = state.io[i];
    if (!io.ie || io.oe || io.od || !gpio_mux_alias(i, io.fun_sel) ||
        io.sig_out != SIG_GPIO_OUT_IDX || io.oe_inv ||
        GPIO.pin[pin].int_type != GPIO_INTR_ANYEDGE || GPIO.pin[pin].int_ena == 0) {
      std::snprintf(reason, capacity,
                    "GPIO%d preflight: IE%u OE%u OD%u F%u S%u OEi%u OEc%u IRQ%u EN%u; require IE1 OE0 OD0 GPIO alias0/2 S256 OEi0 IRQ3 EN>0",
                    static_cast<int>(pin), static_cast<unsigned>(io.ie),
                    static_cast<unsigned>(io.oe), static_cast<unsigned>(io.od),
                    static_cast<unsigned>(io.fun_sel), static_cast<unsigned>(io.sig_out),
                    static_cast<unsigned>(io.oe_inv), static_cast<unsigned>(io.oe_ctrl_by_periph),
                    static_cast<unsigned>(GPIO.pin[pin].int_type),
                    static_cast<unsigned>(GPIO.pin[pin].int_ena));
      return false;
    }
    if (!GPIO.func_in_sel_cfg[RX_SIGNALS[i]].sig_in_sel ||
        GPIO.func_in_sel_cfg[RX_SIGNALS[i]].sig_in_inv ||
        GPIO.func_in_sel_cfg[RX_SIGNALS[i]].func_sel != static_cast<uint32_t>(pin)) {
      std::snprintf(reason, capacity, "UART%d RX matrix is not GPIO%d normal",
                    static_cast<int>(PORTS[i]), static_cast<int>(pin));
      return false;
    }
    uint32_t baud = 0;
    uart_word_length_t bits{};
    uart_parity_t parity{};
    uart_stop_bits_t stops{};
    uart_hw_flowcontrol_t flow{};
    auto *hw = UART_LL_GET_HW(PORTS[i]);
    if (!uart_is_driver_installed(PORTS[i]) ||
        uart_get_baudrate(PORTS[i], &baud) != ESP_OK ||
        uart_get_word_length(PORTS[i], &bits) != ESP_OK ||
        uart_get_parity(PORTS[i], &parity) != ESP_OK ||
        uart_get_stop_bits(PORTS[i], &stops) != ESP_OK ||
        uart_get_hw_flow_ctrl(PORTS[i], &flow) != ESP_OK ||
        baud != 9600 || bits != UART_DATA_8_BITS || parity != UART_PARITY_DISABLE ||
        stops != UART_STOP_BITS_1 || flow != UART_HW_FLOWCTRL_DISABLE ||
        hw->conf0.txd_inv || hw->conf0.rxd_inv || hw->conf0.txd_brk || hw->conf0.loopback ||
        !uart_ll_is_tx_idle(hw)) {
      std::snprintf(reason, capacity, "UART%d requires idle 9600 8N1 normal, no flow/BREAK/loopback",
                    static_cast<int>(PORTS[i]));
      return false;
    }
  }
  return true;
}

// Refuse a UART signal already routed to another output. No pins other than
// the selected GPIO are changed. Also check the UART's native IOMUX TX pad.
inline bool no_other_tx_pad(size_t index) {
  const uint32_t signal = TX_SIGNALS[index];
  for (int gpio = 0; gpio < 34; ++gpio) {
    if (gpio == static_cast<int>(PINS[index]) || !GPIO_IS_VALID_OUTPUT_GPIO(gpio))
      continue;
    gpio_io_config_t io{};
    if (gpio_get_io_config(static_cast<gpio_num_t>(gpio), &io) != ESP_OK)
      return false;
    if (io.fun_sel == PIN_FUNC_GPIO && io.sig_out == signal)
      return false;
  }
  const auto &native = uart_periph_signal[PORTS[index]].pins[SOC_UART_TX_PIN_IDX];
  if (native.default_gpio >= 0 && native.iomux_func != PIN_FUNC_GPIO) {
    gpio_io_config_t io{};
    if (gpio_get_io_config(static_cast<gpio_num_t>(native.default_gpio), &io) != ESP_OK ||
        io.fun_sel == static_cast<uint32_t>(native.iomux_func))
      return false;
  }
  return true;
}

class OutputGuard {
 public:
  OutputGuard(size_t index, const Snapshot &before) : index_(index), before_(before) {}
  OutputGuard(const OutputGuard &) = delete;
  OutputGuard &operator=(const OutputGuard &) = delete;

  void enable() {
    touched_ = true;
    const auto pin = PINS[index_];
    // Public gpio_set_direction(INPUT_OUTPUT_OD) enables output before OD;
    // esp_rom_gpio_connect_out_signal() also enables pad output. Avoid both.
    // The LL/register sequence below leaves OE disabled until OD and idle-HIGH
    // UART routing have both been established. Input signal routing/enable is
    // unchanged. Canonicalize documented GPIO mux alias 0 to GPIO function 2
    // only while GPIO OE=0, and restore the original function during cleanup.
    gpio_ll_output_disable(&GPIO, pin);
    gpio_ll_set_output_enable_ctrl(&GPIO, pin, false, false);
    gpio_ll_set_level(&GPIO, pin, 1);
    gpio_ll_func_sel(&GPIO, pin, PIN_FUNC_GPIO);
    gpio_ll_od_enable(&GPIO, pin);
    GPIO.func_out_sel_cfg[pin].func_sel = TX_SIGNALS[index_];
    GPIO.func_out_sel_cfg[pin].inv_sel = 0;
    gpio_ll_set_output_enable_ctrl(&GPIO, pin, false, false);
    gpio_ll_output_enable(&GPIO, pin);
  }

  bool restore() {
    if (restored_)
      return restore_ok_;
    restored_ = true;
    if (touched_) {
      const auto pin = PINS[index_];
      // Disconnect the pad BEFORE changing UART routing or clearing OD.
      // All output-route writes below happen with GPIO OE=0. Restoring the
      // original latch is safe only after this output disconnection.
      gpio_ll_set_level(&GPIO, pin, 1);
      gpio_ll_output_disable(&GPIO, pin);
      gpio_ll_set_output_enable_ctrl(&GPIO, pin, false, false);
      GPIO.func_out_sel_cfg[pin].func_sel = SIG_GPIO_OUT_IDX;
      GPIO.func_out_sel_cfg[pin].val = before_.output_route[index_];
      gpio_ll_od_disable(&GPIO, pin);
      gpio_ll_set_level(&GPIO, pin, (before_.output_latch >> pin) & 1U);
      // Both accepted aliases are GPIO functions. Restoring the mux happens
      // only after OE=0 and after the output signal has been disconnected.
      gpio_ll_func_sel(&GPIO, pin, before_.io[index_].fun_sel);
      gpio_ll_input_enable(&GPIO, pin);
    }
    restore_ok_ = true;
    for (size_t i = 0; i < PINS.size(); ++i) {
      gpio_io_config_t after{};
      restore_ok_ = (gpio_get_io_config(PINS[i], &after) == ESP_OK &&
                     same_io(before_.io[i], after) &&
                     GPIO.func_out_sel_cfg[PINS[i]].val == before_.output_route[i] &&
                     GPIO.func_in_sel_cfg[RX_SIGNALS[i]].val == before_.rx_route[i] &&
                     GPIO.pin[PINS[i]].val == before_.irq_config[i]) && restore_ok_;
    }
    return restore_ok_;
  }

  ~OutputGuard() { restore(); }

 private:
  size_t index_;
  const Snapshot &before_;
  bool touched_{false};
  bool restored_{false};
  bool restore_ok_{false};
};

}  // namespace detail

// Read-only inspection: does not arm, configure, route, enable an output or
// transmit anything. RX triplet is actual GPIO source / matrix select / invert;
// IRQ/EN are the current interrupt type and CPU-enable mask, not saved defaults.
// Keep both pads compact enough for a normal ESPHome text-sensor state.
inline std::string describe() {
  std::string result;
  result.reserve(250);
  for (size_t i = 0; i < detail::PINS.size(); ++i) {
    const auto pin = detail::PINS[i];
    const uint32_t signal = detail::RX_SIGNALS[i];
    gpio_io_config_t io{};
    const esp_err_t err = gpio_get_io_config(pin, &io);
    char line[220];
    if (err != ESP_OK) {
      std::snprintf(line, sizeof(line), "G%d{config=%s}", static_cast<int>(pin), esp_err_to_name(err));
    } else {
      std::snprintf(line, sizeof(line),
                    "G%d{IE%u OE%u OD%u F%u S%u OEi%u OEc%u Oi%u IRQ%u EN%u RX%u/%u/%u L%d PU%u PD%u D%u SL%u}",
                    static_cast<int>(pin), static_cast<unsigned>(io.ie),
                    static_cast<unsigned>(io.oe), static_cast<unsigned>(io.od),
                    static_cast<unsigned>(io.fun_sel), static_cast<unsigned>(io.sig_out),
                    static_cast<unsigned>(io.oe_inv), static_cast<unsigned>(io.oe_ctrl_by_periph),
                    static_cast<unsigned>(GPIO.func_out_sel_cfg[pin].inv_sel),
                    static_cast<unsigned>(GPIO.pin[pin].int_type),
                    static_cast<unsigned>(GPIO.pin[pin].int_ena),
                    static_cast<unsigned>(GPIO.func_in_sel_cfg[signal].func_sel),
                    static_cast<unsigned>(GPIO.func_in_sel_cfg[signal].sig_in_sel),
                    static_cast<unsigned>(GPIO.func_in_sel_cfg[signal].sig_in_inv),
                    gpio_get_level(pin), static_cast<unsigned>(io.pu), static_cast<unsigned>(io.pd),
                    static_cast<unsigned>(io.drv), static_cast<unsigned>(io.slp_sel));
    }
    if (i)
      result += "; ";
    result += line;
  }
  return result;
}

namespace detail {

// Common transport for whitelisted reads and ONLY the two fixed setpoint
// steps. Exactly one frame per call, without retry; no raw-data entry point.
inline std::string transmit(size_t index, Operation operation, uint16_t address) {
  if (index != 0)
    return "REFUSED: verified TX direction is index 0/GPIO25 only; no output changed";
  if (operation != Operation::READ && operation != Operation::INCREMENT && operation != Operation::DECREMENT)
    return "REFUSED: unsupported operation; no output changed";
  if (operation == Operation::READ && !detail::readable_register(address))
    return "REFUSED: address outside fixed read-only F6 whitelist; no output changed";
  if (detail::busy.test_and_set(std::memory_order_acquire))
    return "REFUSED: another query is in progress; no output changed";
  detail::CallGuard call;
  detail::Snapshot before;
  char reason[180]{};
  if (!detail::snapshot(before, reason, sizeof(reason)))
    return std::string("REFUSED: ") + reason + "; no output changed";
  if (!detail::no_other_tx_pad(index))
    return "REFUSED: UART TX already routed to another pad or pad check failed; no output changed";

  const auto pin = detail::PINS[index];
  const auto port = detail::PORTS[index];
  // Normal UART requires idle HIGH; do not attempt to override a line that is
  // LOW or already carrying activity. This is not a voltage measurement.
  const int64_t quiet_start = esp_timer_get_time();
  while (esp_timer_get_time() - quiet_start < 2000) {
    if (!gpio_get_level(pin))
      return "REFUSED: selected line is not idle HIGH for 2 ms; no output changed";
    esp_rom_delay_us(20);
  }

  detail::OutputGuard output(index, before);
  std::array<uint8_t, 12> frame{};
  size_t frame_size = 12;
  char command[20];
  if (operation == Operation::READ) {
    frame = detail::read_frame(address);
    std::snprintf(command, sizeof(command), "read %04X", static_cast<unsigned>(address));
  } else {
    const auto &step = operation == Operation::INCREMENT ? detail::INCREMENT : detail::DECREMENT;
    frame_size = step.size();
    for (size_t i = 0; i < frame_size; ++i)
      frame[i] = step[i];
    std::snprintf(command, sizeof(command), "%s", operation == Operation::INCREMENT ? "INC" : "DEC");
  }
  auto *hw = UART_LL_GET_HW(port);
  if (!uart_ll_is_tx_idle(hw))
    return "REFUSED: UART TX became busy; no output changed";
  const int64_t started = esp_timer_get_time();
  output.enable();
  gpio_io_config_t active{};
  auto expected_active = before.io[index];
  expected_active.fun_sel = PIN_FUNC_GPIO;
  expected_active.oe = true;
  expected_active.od = true;
  expected_active.sig_out = detail::TX_SIGNALS[index];
  expected_active.oe_ctrl_by_periph = false;
  expected_active.oe_inv = false;
  if (gpio_get_io_config(pin, &active) != ESP_OK ||
      !detail::same_io(expected_active, active) || !gpio_get_level(pin) ||
      GPIO.func_in_sel_cfg[U1RXD_IN_IDX].val != before.rx_route[0] ||
      GPIO.func_in_sel_cfg[U2RXD_IN_IDX].val != before.rx_route[1]) {
    const bool restored = output.restore();
    return restored ? "FAIL: open-drain/idle/RX-route verification; no query sent; input restored"
                    : "FAIL: open-drain verification; no query sent; restore MISMATCH";
  }
  // No TX ring buffer is installed by ESPHome's IDF RX driver. uart_tx_chars()
  // fills the empty hardware FIFO once (7 or 12 bytes fit its 128-byte FIFO),
  // under the driver's TX mutex. It does not perform byte-by-byte bit banging.
  const int queued = uart_tx_chars(port, reinterpret_cast<const char *>(frame.data()), frame_size);
  // Keep all interrupts enabled; RX ISR/FIFOs can receive a reply while this
  // synchronous function waits. Bound the local hardware wait to 100 ms.
  const int64_t deadline = started + 100000;
  while (queued == static_cast<int>(frame_size) && !uart_ll_is_tx_idle(hw) &&
         esp_timer_get_time() < deadline) {
    esp_rom_delay_us(50);
  }
  const bool tx_done = queued == static_cast<int>(frame_size) && uart_ll_is_tx_idle(hw);
  const bool restored = output.restore();
  const int64_t elapsed = esp_timer_get_time() - started;
  char result[230];
  std::snprintf(result, sizeof(result),
                "%s GPIO%d UART%d %s: local TX %d/%u, idle=%s, elapsed=%lld us; input/RX/IRQ/pulls=%s; verify other-channel response",
                tx_done && restored ? "OK" : "FAIL", static_cast<int>(pin), static_cast<int>(port),
                command, queued, static_cast<unsigned>(frame_size), tx_done ? "yes" : "no",
                static_cast<long long>(elapsed), restored ? "restored" : "MISMATCH");
  return result;
}

}  // namespace detail

// The proven TX direction is index 0 = GPIO25/UART1, receiving on GPIO26/UART2.
// Returned OK describes local TX/restoration, never proof of a heater change.
inline std::string run_register(size_t index, uint16_t address) {
  return detail::transmit(index, detail::Operation::READ, address);
}

// Call only through the verified controller: increase=true is precisely one
// INC frame, false precisely one DEC frame. A subsequent 060E read is mandatory
// before assuming success or considering another step. No automatic retries.
inline std::string run_step(size_t index, bool increase) {
  return detail::transmit(index, increase ? detail::Operation::INCREMENT : detail::Operation::DECREMENT, 0);
}

// Source-compatible manual temperature-query entry point. It shares the same
// fixed TX direction and whitelist gate; inlet=false selects 0604, true 0606.
inline std::string run(size_t index, bool inlet = false) {
  return run_register(index, inlet ? uint16_t{0x0606} : uint16_t{0x0604});
}

}  // namespace rinnai_read_query
