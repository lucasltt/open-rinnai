#pragma once

// Read-response parser and one-outstanding-request bookkeeping only.
// Feed ONLY the heater's verified response channel (GPIO26). This file never
// transmits, configures GPIO/UART, retries, schedules polls, or controls heat.
// Calls must be serialized in ESPHome's main-loop context.
//
// Host tests may define RINNAI_TELEMETRY_MILLIS() before including this header,
// e.g. to return a uint32_t fake clock. Otherwise use ESPHome's normal clock.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#ifndef RINNAI_TELEMETRY_MILLIS
#include "esphome/core/hal.h"
#define RINNAI_TELEMETRY_MILLIS() ::esphome::millis()
#endif

namespace rinnai_telemetry {

struct Reading {
  uint16_t address;
  uint16_t raw;
  std::string data_hex;  // The four ASCII data characters, in wire order.
  uint32_t rtt_ms;      // Includes UART/debug batching and main-loop latency.
};

inline constexpr uint32_t RESPONSE_TIMEOUT_MS = 500;
inline constexpr std::array<uint16_t, 8> READ_ADDRESSES{{
    0x0604, 0x0606, 0x0608, 0x0648, 0x060E, 0x0611, 0x0614, 0x060F}};

namespace detail {

inline uint32_t now_ms() { return static_cast<uint32_t>(RINNAI_TELEMETRY_MILLIS()); }

inline int register_index(uint16_t address) {
  for (size_t index = 0; index < READ_ADDRESSES.size(); ++index) {
    if (READ_ADDRESSES[index] == address)
      return static_cast<int>(index);
  }
  return -1;
}

inline int hex_nibble(uint8_t value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  return -1;
}

class Parser {
 public:
  // A request can start only when there is no unexpired request. Clearing the
  // parser buffer does NOT drain an underlying UART driver/debugger queue;
  // the caller must avoid old queued data and overlapping physical requests.
  // No transaction ID exists on this protocol, so a late reply for the SAME
  // address cannot be distinguished from a new reply solely from its bytes.
  void request_started(uint16_t address) {
    this->timeout_check();
    if (register_index(address) < 0 || this->pending_) {
      ++this->rejected_requests_;
      return;
    }
    this->used_ = 0;
    this->pending_address_ = address;
    this->request_ms_ = now_ms();
    this->pending_ = true;
  }

  bool pending() const { return this->pending_; }

  // Returns true only when this call expires a request; never counts it twice.
  // Unsigned subtraction remains correct across uint32_t millis() rollover.
  bool timeout_check() { return this->expire_(now_ms()); }

  std::vector<Reading> feed(const std::vector<uint8_t> &bytes) {
    this->timeout_check();
    std::vector<Reading> readings;
    // At most one accepted Reading is possible for one outstanding request.
    // The stream buffer itself is fixed; noise cannot grow a heap buffer.
    for (const uint8_t value : bytes) {
      if (value == 0x02) {
        if (this->used_ != 0)
          ++this->bad_frames_;  // A new STX abandons a truncated candidate.
        this->buffer_[0] = value;
        this->used_ = 1;
        continue;
      }
      if (this->used_ == 0)
        continue;  // Noise outside a candidate is not a complete bad frame.
      if (this->used_ == this->buffer_.size()) {
        ++this->bad_frames_;
        this->used_ = 0;
        continue;
      }
      this->buffer_[this->used_++] = value;
      if (value == 0x0D) {
        this->parse_candidate_(readings);
        this->used_ = 0;
      }
    }
    return readings;
  }

  uint32_t valid_frames() const { return this->valid_frames_; }
  uint32_t bad_frames() const { return this->bad_frames_; }
  uint32_t stale_frames() const { return this->stale_frames_; }
  uint32_t control_echo_frames() const { return this->control_echo_frames_; }
  uint32_t rejected_requests() const { return this->rejected_requests_; }
  uint32_t timeouts() const { return this->timeouts_; }
  uint32_t last_valid_ms() const { return this->last_valid_ms_; }
  uint32_t last_rtt_ms() const { return this->last_rtt_ms_; }
  uint32_t last_seen(uint16_t address) const {
    const int index = register_index(address);
    return index < 0 ? 0 : this->last_seen_[static_cast<size_t>(index)];
  }

 private:
  bool expire_(uint32_t now) {
    if (!this->pending_ || static_cast<uint32_t>(now - this->request_ms_) < RESPONSE_TIMEOUT_MS)
      return false;
    this->pending_ = false;
    this->used_ = 0;
    ++this->timeouts_;
    return true;
  }

  void parse_candidate_(std::vector<Reading> &readings) {
    // Physically observed on GPIO26 after one INC/DEC: an exact mirrored
    // 7-byte control frame. Count it separately; it NEVER confirms a setpoint
    // or satisfies a pending read. Only correlated 060E readback does that.
    if (this->used_ == 7 && this->buffer_[0] == 0x02 && this->buffer_[1] == 0x55 &&
        (this->buffer_[2] == 0x30 || this->buffer_[2] == 0x31) && this->buffer_[3] == 0x03 &&
        this->buffer_[4] == '8' && this->buffer_[5] == (this->buffer_[2] == 0x30 ? '8' : '9') &&
        this->buffer_[6] == 0x0D) {
      ++this->control_echo_frames_;
      return;
    }
    // A local 12-byte request echo fails this length check. Never decode it as
    // a heater response. The response length field is ASCII '02', not byte 2.
    if (this->used_ != 16 || this->buffer_[0] != 0x02 || this->buffer_[1] != 0xF6 ||
        this->buffer_[6] != '0' || this->buffer_[7] != '2' || this->buffer_[12] != 0x03 ||
        this->buffer_[15] != 0x0D) {
      ++this->bad_frames_;
      return;
    }
    uint16_t address = 0;
    for (size_t index = 2; index <= 5; ++index) {
      const int nibble = hex_nibble(this->buffer_[index]);
      if (nibble < 0) {
        ++this->bad_frames_;
        return;
      }
      address = static_cast<uint16_t>((address << 4) | static_cast<unsigned>(nibble));
    }
    const int slot = register_index(address);
    if (slot < 0) {
      ++this->bad_frames_;
      return;
    }
    std::array<int, 4> data{};
    for (size_t index = 0; index < data.size(); ++index) {
      data[index] = hex_nibble(this->buffer_[index + 8]);
      if (data[index] < 0) {
        ++this->bad_frames_;
        return;
      }
    }
    const int checksum_high = hex_nibble(this->buffer_[13]);
    const int checksum_low = hex_nibble(this->buffer_[14]);
    uint8_t checksum = 0;
    for (size_t index = 1; index <= 12; ++index)
      checksum = static_cast<uint8_t>(checksum + this->buffer_[index]);
    if (checksum_high < 0 || checksum_low < 0 ||
        checksum != static_cast<uint8_t>((checksum_high << 4) | checksum_low)) {
      ++this->bad_frames_;
      return;
    }

    const uint32_t now = now_ms();
    this->expire_(now);
    if (!this->pending_ || address != this->pending_address_) {
      ++this->stale_frames_;
      return;
    }
    const uint16_t low_byte = static_cast<uint16_t>((data[0] << 4) | data[1]);
    const uint16_t high_byte = static_cast<uint16_t>((data[2] << 4) | data[3]);
    const uint16_t raw = static_cast<uint16_t>(low_byte | (high_byte << 8));
    const uint32_t elapsed = static_cast<uint32_t>(now - this->request_ms_);
    readings.push_back(Reading{address, raw,
                               std::string(reinterpret_cast<const char *>(this->buffer_.data() + 8), 4),
                               elapsed});
    this->pending_ = false;
    ++this->valid_frames_;
    this->last_valid_ms_ = now;
    this->last_rtt_ms_ = elapsed;
    this->last_seen_[static_cast<size_t>(slot)] = now;
  }

  std::array<uint8_t, 64> buffer_{};
  size_t used_{0};
  bool pending_{false};
  uint16_t pending_address_{0};
  uint32_t request_ms_{0};
  uint32_t valid_frames_{0};
  uint32_t bad_frames_{0};
  uint32_t stale_frames_{0};
  uint32_t control_echo_frames_{0};
  uint32_t rejected_requests_{0};
  uint32_t timeouts_{0};
  uint32_t last_valid_ms_{0};
  uint32_t last_rtt_ms_{0};
  std::array<uint32_t, READ_ADDRESSES.size()> last_seen_{};
};

inline Parser parser{};

}  // namespace detail

inline std::function<void(const Reading &)> reading_callback{};
inline bool background_paused_ = false;
inline void set_on_reading(std::function<void(const Reading &)> callback) { reading_callback = std::move(callback); }
inline void set_background_paused(bool paused) { background_paused_ = paused; }
inline bool background_paused() { return background_paused_; }
inline std::vector<Reading> feed(const std::vector<uint8_t> &bytes) {
  auto readings = detail::parser.feed(bytes);
  if (reading_callback)
    for (const auto &reading : readings) reading_callback(reading);
  return readings;
}
inline void request_started(uint16_t address) { detail::parser.request_started(address); }
inline bool pending() { return detail::parser.pending(); }
inline bool timeout_check() { return detail::parser.timeout_check(); }
inline uint32_t valid_frames() { return detail::parser.valid_frames(); }
inline uint32_t bad_frames() { return detail::parser.bad_frames(); }
inline uint32_t stale_frames() { return detail::parser.stale_frames(); }
inline uint32_t control_echo_frames() { return detail::parser.control_echo_frames(); }
inline uint32_t rejected_requests() { return detail::parser.rejected_requests(); }
inline uint32_t timeouts() { return detail::parser.timeouts(); }
inline uint32_t last_valid_ms() { return detail::parser.last_valid_ms(); }
inline uint32_t last_rtt_ms() { return detail::parser.last_rtt_ms(); }
inline uint32_t last_seen(uint16_t address) { return detail::parser.last_seen(address); }

}  // namespace rinnai_telemetry
