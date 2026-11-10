#pragma once
#include <cstdint>
#include <cstring>

// Host-only deterministic entropy; never compiled into the ESP firmware.
namespace esphome {
inline uint32_t test_random_state = 0x7A931C5BU;
inline uint32_t random_uint32() {
  auto value = test_random_state;
  value ^= value << 13U; value ^= value >> 17U; value ^= value << 5U;
  test_random_state = value;
  return value;
}
inline void get_mac_address_raw(uint8_t *mac) {
  const uint8_t example[6]{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  std::memcpy(mac, example, sizeof(example));
}
}  // namespace esphome
