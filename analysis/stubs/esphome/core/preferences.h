#pragma once

// Host-only durable/pending preference stores. A simulated power loss discards
// pending saves; only a successful sync commits their bytes to durable storage.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <type_traits>
#include <vector>

namespace esphome {
class ESPPreferences;

class ESPPreferenceObject {
 public:
  ESPPreferenceObject() = default;
  ESPPreferenceObject(ESPPreferences *owner, uint32_t key, size_t length)
      : owner_(owner), key_(key), length_(length) {}

  template<typename T> bool load(T *value) const;
  template<typename T> bool save(const T *value);

 private:
  ESPPreferences *owner_{nullptr};
  uint32_t key_{0};
  size_t length_{0};
};

class ESPPreferences {
 public:
  std::map<uint32_t, std::vector<uint8_t>> durable;
  std::map<uint32_t, std::vector<uint8_t>> pending;
  bool allow_save{true};
  bool allow_sync{true};
  unsigned save_calls{0};
  unsigned sync_calls{0};

  template<typename T> ESPPreferenceObject make_preference(uint32_t key) {
    static_assert(std::is_trivially_copyable<T>::value, "Preference must be POD");
    return ESPPreferenceObject(this, key, sizeof(T));
  }

  bool sync() {
    ++sync_calls;
    if (!allow_sync)
      return false;
    for (const auto &item : pending)
      durable[item.first] = item.second;
    pending.clear();
    return true;
  }

  template<typename T> void seed(uint32_t key, const T &value) {
    std::vector<uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    durable[key] = bytes;
  }

  template<typename T> bool read(uint32_t key, T &value) const {
    const auto found = durable.find(key);
    if (found == durable.end() || found->second.size() != sizeof(T))
      return false;
    std::memcpy(&value, found->second.data(), sizeof(T));
    return true;
  }
};

template<typename T> bool ESPPreferenceObject::load(T *value) const {
  return owner_ != nullptr && value != nullptr && length_ == sizeof(T) && owner_->read(key_, *value);
}

template<typename T> bool ESPPreferenceObject::save(const T *value) {
  if (owner_ == nullptr || value == nullptr || length_ != sizeof(T))
    return false;
  ++owner_->save_calls;
  if (!owner_->allow_save)
    return false;
  std::vector<uint8_t> bytes(sizeof(T));
  std::memcpy(bytes.data(), value, sizeof(T));
  owner_->pending[key_] = bytes;
  return true;
}

inline ESPPreferences *global_preferences = nullptr;
}  // namespace esphome
