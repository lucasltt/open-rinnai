#pragma once

// Day/month summaries of observed counter deltas. No clock service, network,
// storage or peripheral dependency. Local dates use a fixed UTC offset (Brazil
// UTC-03 by default). Seed after every boot: restored legacy totals and the
// unobserved reboot interval are never allocated retrospectively to a date.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "heater_stats.h"

namespace heater_periods {

inline constexpr size_t DAY_CAPACITY = 31;
inline constexpr size_t MONTH_CAPACITY = 12;
inline constexpr uint64_t MIN_EPOCH_S = 1577836800ULL;  // 2020-01-01 UTC.
inline constexpr uint64_t MAX_EPOCH_S = 253402300799ULL;  // 9999-12-31 UTC.
inline constexpr uint64_t MAX_INTERVAL_S = 30;
inline constexpr int32_t DEFAULT_OFFSET_SECONDS = -10800;

inline bool clock_valid(uint64_t epoch_s) { return epoch_s >= MIN_EPOCH_S && epoch_s <= MAX_EPOCH_S; }

struct Bucket {
  double water_l{0};
  double energy_kwh{0};
  uint32_t key{0};  // YYYYMMDD for days; YYYYMM for months.
  uint32_t heating_seconds{0};
  uint32_t sessions{0};
  uint8_t partial{0};
  uint8_t reserved[3]{0, 0, 0};
};
static_assert(sizeof(Bucket) == 32 && offsetof(Bucket, key) == 16 && offsetof(Bucket, partial) == 28,
              "Stable bucket layout without implicit padding");

struct Snapshot {
  uint32_t version{1};
  int32_t offset_seconds{DEFAULT_OFFSET_SECONDS};
  uint64_t tracked_since_epoch{0};
  heater_stats::Totals unassigned{};  // Observed deltas whose date was uncertain.
  uint16_t day_count{0}, month_count{0};
  uint32_t reserved{0};
  std::array<Bucket, DAY_CAPACITY> days{};  // Oldest first, unused rows zero.
  std::array<Bucket, MONTH_CAPACITY> months{};
};
static_assert(std::is_trivially_copyable<Snapshot>::value && std::is_standard_layout<Snapshot>::value,
              "Periods snapshot must be an NVS-copyable scalar layout");
static_assert(sizeof(heater_stats::Totals) == 24 && sizeof(Snapshot) == 1424 && offsetof(Snapshot, days) == 48,
              "Stable periods snapshot layout without implicit padding");

namespace detail {
inline bool valid_totals(const heater_stats::Totals &value) {
  return std::isfinite(value.water_l) && value.water_l >= 0 &&
         std::isfinite(value.energy_kwh) && value.energy_kwh >= 0;
}
inline uint32_t saturated_add(uint32_t a, uint32_t b) { return b > UINT32_MAX - a ? UINT32_MAX : a + b; }
inline void add_amount(double &target, double amount) {
  if (std::isfinite(amount) && amount >= 0 && std::isfinite(target + amount)) target += amount;
}
inline bool leap(int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
inline unsigned month_days(int year, unsigned month) {
  constexpr unsigned lengths[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month >= 1 && month <= 12 ? lengths[month - 1] + (month == 2 && leap(year) ? 1U : 0U) : 0;
}
// Proleptic Gregorian calendar; epoch day zero is 1970-01-01.
inline int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned adjusted = static_cast<unsigned>(static_cast<int>(month) + (month > 2 ? -3 : 9));
  const unsigned doy = (153 * adjusted + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + doe - 719468;
}
struct Date { int year; unsigned month, day; };
inline Date civil_from_days(int64_t day) {
  day += 719468;
  const int64_t era = (day >= 0 ? day : day - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(day - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int year = static_cast<int>(yoe) + static_cast<int>(era * 400);
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned date = doy - (153 * mp + 2) / 5 + 1;
  const unsigned month = static_cast<unsigned>(static_cast<int>(mp) + (mp < 10 ? 3 : -9));
  year += month <= 2;
  return Date{year, month, date};
}
inline bool valid_key(uint32_t key, bool month_only) {
  const int year = static_cast<int>(month_only ? key / 100U : key / 10000U);
  const unsigned month = month_only ? key % 100U : (key / 100U) % 100U;
  const unsigned day = month_only ? 1 : key % 100U;
  return year >= 1970 && year <= 9999 && day >= 1 && day <= month_days(year, month);
}
inline bool valid_bucket(const Bucket &bucket, bool month_only) {
  return valid_key(bucket.key, month_only) && std::isfinite(bucket.water_l) && bucket.water_l >= 0 &&
         std::isfinite(bucket.energy_kwh) && bucket.energy_kwh >= 0 && bucket.partial <= 1 &&
         bucket.reserved[0] == 0 && bucket.reserved[1] == 0 && bucket.reserved[2] == 0;
}
template<size_t N> inline bool valid_rows(const std::array<Bucket, N> &rows, size_t count, bool month_only) {
  if (count > N) return false;
  for (size_t i = 0; i < count; ++i)
    if (!valid_bucket(rows[i], month_only) || (i && rows[i - 1].key >= rows[i].key)) return false;
  const Bucket blank{};
  for (size_t i = count; i < N; ++i)
    if (std::memcmp(&rows[i], &blank, sizeof(blank)) != 0) return false;
  return true;
}
}  // namespace detail

class Periods {
 public:
  explicit Periods(int32_t offset_seconds = DEFAULT_OFFSET_SECONDS)
      : offset_seconds_(offset_seconds >= -86400 && offset_seconds <= 86400 ? offset_seconds : DEFAULT_OFFSET_SECONDS) {}

  void seed(const heater_stats::Totals &totals, uint64_t epoch_s) {
    if (!detail::valid_totals(totals)) return;
    baseline_ = totals;
    seeded_ = true;
    last_clock_valid_ = clock_valid(epoch_s);
    last_epoch_ = last_clock_valid_ ? epoch_s : 0;
    if (last_clock_valid_) {
      const bool continuation = tracked_since_epoch_ != 0;
      if (tracked_since_epoch_ == 0) tracked_since_epoch_ = epoch_s;
      touch_(epoch_s, continuation || epoch_s != day_start_(epoch_s),
             continuation || epoch_s != month_start_(epoch_s));
    }
  }

  // Return value means in-memory content changed; it is not a request to write
  // flash every second. The caller retains its normal checkpoint cadence.
  bool observe(const heater_stats::Totals &totals, uint64_t epoch_s) {
    if (!detail::valid_totals(totals)) {
      last_clock_valid_ = false;
      return mark_last_partial_();
    }
    if (!seeded_) { seed(totals, epoch_s); return true; }
    const bool rebased = totals.water_l < baseline_.water_l || totals.energy_kwh < baseline_.energy_kwh ||
                         totals.heating_seconds < baseline_.heating_seconds || totals.sessions < baseline_.sessions;
    heater_stats::Totals delta{};
    if (!rebased) {
      delta.water_l = totals.water_l - baseline_.water_l;
      delta.energy_kwh = totals.energy_kwh - baseline_.energy_kwh;
      delta.heating_seconds = totals.heating_seconds - baseline_.heating_seconds;
      delta.sessions = totals.sessions - baseline_.sessions;
    }
    baseline_ = totals;
    const bool valid = clock_valid(epoch_s);
    const bool forward = valid && epoch_s >= last_epoch_;
    const bool continuous = !rebased && last_clock_valid_ && forward && epoch_s - last_epoch_ <= MAX_INTERVAL_S;
    bool changed = false;
    if (continuous) {
      changed = allocate_(delta, last_epoch_, epoch_s);
    } else {
      changed = mark_last_partial_();
      if (!rebased) {
        detail::add_amount(unassigned_.water_l, delta.water_l);
        detail::add_amount(unassigned_.energy_kwh, delta.energy_kwh);
        unassigned_.heating_seconds = detail::saturated_add(unassigned_.heating_seconds, delta.heating_seconds);
        unassigned_.sessions = detail::saturated_add(unassigned_.sessions, delta.sessions);
        changed = changed || nonzero_(delta);
      }
      if (valid) changed = touch_(epoch_s, true, true) || changed;
    }
    if (valid && tracked_since_epoch_ == 0) { tracked_since_epoch_ = epoch_s; changed = true; }
    // Do not attribute consumption to regressed dates while the clock catches
    // its previous high-water mark. Counter baseline still advances each call.
    last_clock_valid_ = forward;
    if (forward) last_epoch_ = epoch_s;
    return changed;
  }

  size_t day_count() const { return day_count_; }
  size_t month_count() const { return month_count_; }
  Bucket day(size_t index) const { return index < day_count_ ? days_[index] : Bucket{}; }
  Bucket month_record(size_t index) const { return index < month_count_ ? months_[index] : Bucket{}; }
  Bucket today(uint64_t epoch_s) const { return current_(days_, day_count_, epoch_s, false); }
  Bucket month(uint64_t epoch_s) const { return current_(months_, month_count_, epoch_s, true); }
  uint64_t tracked_since_epoch() const { return tracked_since_epoch_; }
  heater_stats::Totals unassigned() const { return unassigned_; }
  int32_t offset_seconds() const { return offset_seconds_; }

  Snapshot snapshot() const {
    Snapshot saved{};
    saved.offset_seconds = offset_seconds_;
    saved.tracked_since_epoch = tracked_since_epoch_;
    saved.unassigned = unassigned_;
    saved.day_count = static_cast<uint16_t>(day_count_);
    saved.month_count = static_cast<uint16_t>(month_count_);
    saved.days = days_;
    saved.months = months_;
    return saved;
  }
  bool restore(const Snapshot &saved) {
    if (saved.version != 1 || saved.reserved != 0 || saved.offset_seconds < -86400 || saved.offset_seconds > 86400 ||
        (saved.tracked_since_epoch != 0 && !clock_valid(saved.tracked_since_epoch)) ||
        !detail::valid_totals(saved.unassigned) || !detail::valid_rows(saved.days, saved.day_count, false) ||
        !detail::valid_rows(saved.months, saved.month_count, true)) return false;
    offset_seconds_ = saved.offset_seconds;
    tracked_since_epoch_ = saved.tracked_since_epoch;
    unassigned_ = saved.unassigned;
    days_ = saved.days;
    months_ = saved.months;
    day_count_ = saved.day_count;
    month_count_ = saved.month_count;
    seeded_ = false;
    last_epoch_ = 0;
    last_clock_valid_ = false;
    baseline_ = {};
    return true;
  }

 private:
  static bool nonzero_(const heater_stats::Totals &totals) {
    return totals.water_l != 0 || totals.energy_kwh != 0 || totals.heating_seconds != 0 || totals.sessions != 0;
  }
  int64_t local_day_(uint64_t epoch_s) const { return (static_cast<int64_t>(epoch_s) + offset_seconds_) / 86400; }
  detail::Date date_(uint64_t epoch_s) const { return detail::civil_from_days(local_day_(epoch_s)); }
  uint32_t key_(uint64_t epoch_s, bool month_only) const {
    const auto date = date_(epoch_s);
    return month_only ? static_cast<uint32_t>(date.year) * 100U + date.month :
      static_cast<uint32_t>(date.year) * 10000U + date.month * 100U + date.day;
  }
  uint64_t day_start_(uint64_t epoch_s) const { return static_cast<uint64_t>(local_day_(epoch_s) * 86400 - offset_seconds_); }
  uint64_t month_start_(uint64_t epoch_s) const {
    const auto date = date_(epoch_s);
    return static_cast<uint64_t>(detail::days_from_civil(date.year, date.month, 1) * 86400 - offset_seconds_);
  }
  template<size_t N> Bucket current_(const std::array<Bucket, N> &rows, size_t count, uint64_t epoch_s, bool monthly) const {
    if (!clock_valid(epoch_s)) return Bucket{};
    const uint32_t key = key_(epoch_s, monthly);
    for (size_t i = 0; i < count; ++i) if (rows[i].key == key) return rows[i];
    Bucket empty{}; empty.key = key; empty.partial = 1; return empty;
  }
  template<size_t N> static Bucket *touch_row_(std::array<Bucket, N> &rows, size_t &count, uint32_t key,
                                              bool partial, bool &changed, bool force_partial = false) {
    for (size_t i = 0; i < count; ++i) if (rows[i].key == key) {
      if (force_partial && partial && !rows[i].partial) { rows[i].partial = 1; changed = true; }
      return &rows[i];
    }
    if (count == N) {
      if (key < rows[0].key) return nullptr;
      for (size_t i = 1; i < count; ++i) rows[i - 1] = rows[i];
      --count;
      rows[count] = Bucket{};
    }
    size_t index = count;
    while (index && rows[index - 1].key > key) { rows[index] = rows[index - 1]; --index; }
    rows[index] = Bucket{};
    rows[index].key = key;
    rows[index].partial = partial ? 1 : 0;
    ++count;
    changed = true;
    return &rows[index];
  }
  bool touch_(uint64_t epoch_s, bool day_partial, bool month_partial) {
    if (!clock_valid(epoch_s)) return false;
    bool changed = false;
    touch_row_(days_, day_count_, key_(epoch_s, false), day_partial, changed, true);
    touch_row_(months_, month_count_, key_(epoch_s, true), month_partial, changed, true);
    return changed;
  }
  bool mark_last_partial_() { return clock_valid(last_epoch_) ? touch_(last_epoch_, true, true) : false; }
  static void add_(Bucket *bucket, double water_l, double energy_kwh, uint32_t heating, uint32_t sessions) {
    if (!bucket) return;
    detail::add_amount(bucket->water_l, water_l);
    detail::add_amount(bucket->energy_kwh, energy_kwh);
    bucket->heating_seconds = detail::saturated_add(bucket->heating_seconds, heating);
    bucket->sessions = detail::saturated_add(bucket->sessions, sessions);
  }
  bool allocate_(const heater_stats::Totals &delta, uint64_t start, uint64_t end) {
    bool changed = false;
    if (start == end) {
      auto *day = touch_row_(days_, day_count_, key_(end, false), end != day_start_(end), changed);
      auto *month = touch_row_(months_, month_count_, key_(end, true), end != month_start_(end), changed);
      add_(day, delta.water_l, delta.energy_kwh, delta.heating_seconds, delta.sessions);
      add_(month, delta.water_l, delta.energy_kwh, delta.heating_seconds, delta.sessions);
      return changed || nonzero_(delta);
    }
    uint64_t cursor = start;
    double assigned_water = 0, assigned_energy = 0;
    uint32_t assigned_heat = 0;
    while (cursor < end) {
      const uint64_t boundary = day_start_(cursor) + 86400;
      const uint64_t until = end < boundary ? end : boundary;
      const double fraction = static_cast<double>(until - start) / static_cast<double>(end - start);
      const double water_to = until == end ? delta.water_l : delta.water_l * fraction;
      const double energy_to = until == end ? delta.energy_kwh : delta.energy_kwh * fraction;
      const uint32_t heat_to = until == end ? delta.heating_seconds :
        static_cast<uint32_t>(static_cast<uint64_t>(delta.heating_seconds) * (until - start) / (end - start));
      auto *day = touch_row_(days_, day_count_, key_(cursor, false), cursor != day_start_(cursor), changed);
      auto *month = touch_row_(months_, month_count_, key_(cursor, true), cursor != month_start_(cursor), changed);
      add_(day, water_to - assigned_water, energy_to - assigned_energy, heat_to - assigned_heat, 0);
      add_(month, water_to - assigned_water, energy_to - assigned_energy, heat_to - assigned_heat, 0);
      assigned_water = water_to; assigned_energy = energy_to; assigned_heat = heat_to;
      cursor = until;
    }
    auto *day = touch_row_(days_, day_count_, key_(end, false), end != day_start_(end), changed);
    auto *month = touch_row_(months_, month_count_, key_(end, true), end != month_start_(end), changed);
    add_(day, 0, 0, 0, delta.sessions);
    add_(month, 0, 0, 0, delta.sessions);
    return changed || nonzero_(delta);
  }

  int32_t offset_seconds_{DEFAULT_OFFSET_SECONDS};
  uint64_t tracked_since_epoch_{0};
  heater_stats::Totals unassigned_{};
  std::array<Bucket, DAY_CAPACITY> days_{};
  std::array<Bucket, MONTH_CAPACITY> months_{};
  size_t day_count_{0}, month_count_{0};
  heater_stats::Totals baseline_{};
  uint64_t last_epoch_{0};
  bool seeded_{false}, last_clock_valid_{false};
};

}  // namespace heater_periods
