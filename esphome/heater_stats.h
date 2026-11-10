#pragma once

// Local, estimated accounting from continuously observed telemetry only.
// No peripherals, network, NVS, transmission, wall-clock inference, or polling.
// Call observe() on newly received records and tick() about once per second.
// observe() first accounts OLD values up to its arrival time, so a new reading
// is never applied retrospectively to time before it was received.
//
// Define HEATER_STATS_MILLIS() before inclusion for deterministic host tests.
// Prices are applied at snapshot time using CURRENT settings; they are not a
// ledger of historical tariffs. History and plot points are RAM-only and reset
// on restore/reboot. Persist contains cumulative counters, not prior sessions.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#ifndef HEATER_STATS_MILLIS
#include "esphome/core/hal.h"
#define HEATER_STATS_MILLIS() ::esphome::millis()
#endif

namespace heater_stats {

inline constexpr uint32_t FRESH_MS = 30000;
inline constexpr uint32_t MAX_INTEGRATION_GAP_MS = 5000;
inline constexpr uint32_t POINT_INTERVAL_MS = 120000;
inline constexpr size_t POINT_CAPACITY = 720;
inline constexpr size_t SESSION_CAPACITY = 24;
inline constexpr double SESSION_START_L_MIN = 0.3;
inline constexpr double SESSION_STOP_L_MIN = 0.05;
// International Table calorie: 1 kcal_IT = 4.1868 kJ.
inline constexpr double KJ_PER_KCAL = 4.1868;
inline constexpr double GN_KCAL_PER_M3 = 9400.0;
inline constexpr double GLP_KCAL_PER_KG = 11500.0;
inline constexpr double UNKNOWN = std::numeric_limits<double>::quiet_NaN();

enum class Signal : uint8_t { FLOW_L_MIN, POWER_KCAL_MIN, INLET_C, OUTLET_C, FLAME };
enum class GasType : uint8_t { GN, GLP };
enum class EndReason : uint8_t { NONE, FLOW_STOP, STALE_FLOW, INTEGRATION_GAP };

struct Totals {
  double water_l{0};
  double energy_kwh{0};
  uint32_t heating_seconds{0};
  uint32_t sessions{0};  // Ended sessions, including explicitly incomplete ones.
};

struct Session {
  uint32_t sequence{0};
  uint32_t start_ms{0};  // Uptime in this boot, never a Unix timestamp.
  uint32_t end_ms{0};
  uint32_t duration_ms{0};  // Observed fresh-flow time, not end_ms-start_ms.
  uint32_t heating_seconds{0};
  double water_l{0};
  double energy_kwh{0};
  double max_flow_l_min{0};
  double max_power_kcal_min{0};
  bool active{false};
  bool complete{false};
  EndReason end_reason{EndReason::NONE};
};

struct Point {
  uint32_t ms{0};
  double flow_l_min{UNKNOWN};
  double power_kcal_min{UNKNOWN};
  double inlet_c{UNKNOWN};
  double outlet_c{UNKNOWN};
  int8_t flame{-1};  // -1 = unknown/stale; serialize as JSON null.
};

struct Settings {
  GasType gas_type{GasType::GN};
  double gas_tariff_per_unit{0};  // R$/m3 for GN, R$/kg for GLP; 0 = unset.
  double water_tariff_per_m3{0};  // R$/m3; 0 = unset.
};

struct Estimate {
  double gas_units{0};
  double water_m3{0};
  double gas_cost{UNKNOWN};
  double water_cost{UNKNOWN};
  double total_cost{UNKNOWN};
  bool gas_cost_valid{false};
  bool water_cost_valid{false};
  bool cost_valid{false};
};

struct Persist {
  double water_l{0};
  double energy_kwh{0};
  uint32_t version{1};
  uint32_t heating_seconds{0};
  uint32_t sessions{0};
  uint16_t heating_remainder_ms{0};
  uint16_t reserved{0};  // Explicit tail field avoids indeterminate NVS padding.
};
static_assert(std::is_trivially_copyable<Persist>::value, "Persist must remain an NVS-copyable POD");
static_assert(std::is_standard_layout<Persist>::value, "Persist must keep a standard scalar layout");
static_assert(sizeof(Persist) == 32, "Persist layout must not acquire implicit padding");

inline const char *end_reason_name(EndReason reason) {
  switch (reason) {
    case EndReason::FLOW_STOP: return "flow_stop";
    case EndReason::STALE_FLOW: return "stale_flow";
    case EndReason::INTEGRATION_GAP: return "integration_gap";
    default: return "none";
  }
}

namespace detail {

inline uint32_t now_ms() { return static_cast<uint32_t>(HEATER_STATS_MILLIS()); }
inline uint32_t smaller(uint32_t a, uint32_t b) { return a < b ? a : b; }
inline uint32_t larger(uint32_t a, uint32_t b) { return a > b ? a : b; }
inline uint32_t saturating_add(uint32_t a, uint32_t b) {
  return b > UINT32_MAX - a ? UINT32_MAX : a + b;
}
inline bool valid_amount(double value) { return std::isfinite(value) && value >= 0; }
inline void add_amount(double &target, double value) {
  const double result = target + value;
  if (valid_amount(value) && std::isfinite(result))
    target = result;
}

struct Sample {
  double value{0};
  uint32_t seen_ms{0};
  bool seen{false};
  bool valid{false};
};

// Match the heater's original register resolution without keeping four doubles
// per stored point. The 24-hour ring occupies the same 11,520 bytes as the old
// 240-point ring of 48-byte Points. Public point() still returns decoded Points.
struct CompactPoint {
  uint32_t ms{0};
  uint16_t inlet_centi{0}, outlet_centi{0}, flow_centi{0}, power_deci{0};
  uint8_t valid_mask{0};
  int8_t flame{-1};
  uint16_t reserved{0};
};
static_assert(sizeof(CompactPoint) == 16 && offsetof(CompactPoint, valid_mask) == 12,
              "Plot points must keep their compact 16-byte scalar layout");
static_assert(std::is_trivially_copyable<CompactPoint>::value &&
              std::is_standard_layout<CompactPoint>::value, "Compact plot storage must remain scalar");
inline constexpr uint8_t PLOT_INLET = 1, PLOT_OUTLET = 2, PLOT_FLOW = 4, PLOT_POWER = 8;

inline bool plot_quantize(double value, double scale, uint16_t &out) {
  if (!std::isfinite(value) || value < 0 || value > 65535.0 / scale) return false;
  out = static_cast<uint16_t>(std::lround(value * scale));
  return true;
}

inline Point decode_point(const CompactPoint &stored) {
  Point point;
  point.ms = stored.ms;
  if (stored.valid_mask & PLOT_INLET) point.inlet_c = stored.inlet_centi / 100.0;
  if (stored.valid_mask & PLOT_OUTLET) point.outlet_c = stored.outlet_centi / 100.0;
  if (stored.valid_mask & PLOT_FLOW) point.flow_l_min = stored.flow_centi / 100.0;
  if (stored.valid_mask & PLOT_POWER) point.power_kcal_min = stored.power_deci / 10.0;
  point.flame = stored.flame;
  return point;
}

class Accounting {
 public:
  void observe(Signal signal, double value, uint32_t seen_ms) {
    const size_t slot = static_cast<size_t>(signal);
    if (slot >= this->samples_.size())
      return;
    const uint32_t now = now_ms();
    Sample &sample = this->samples_[slot];
    // Ignore duplicate/older records and timestamps lying in the future.
    if (static_cast<int32_t>(now - seen_ms) < 0 ||
        (sample.seen && static_cast<int32_t>(seen_ms - sample.seen_ms) <= 0))
      return;
    this->advance_(now);
    // Close the previous plot interval before installing an observation that
    // arrived after its boundary. A late call emits one point, never backfills.
    this->emit_plot_if_due_(now);
    sample.value = value;
    sample.seen_ms = seen_ms;
    sample.seen = true;
    sample.valid = std::isfinite(value) &&
                   ((signal != Signal::FLOW_L_MIN && signal != Signal::POWER_KCAL_MIN) || value >= 0);
    this->observe_plot_(signal, sample, now);
    if (signal == Signal::FLOW_L_MIN) {
      if (!this->fresh_(sample, now)) {
        this->finish_(now, EndReason::STALE_FLOW);
      } else if (value >= SESSION_START_L_MIN && !this->current_.active) {
        this->current_ = Session{};
        this->current_.active = true;
        this->current_.sequence = saturating_add(this->totals_.sessions, 1);
        this->current_.start_ms = now;
        this->current_.max_flow_l_min = value;
        this->session_heat_remainder_ms_ = 0;
      } else if (value <= SESSION_STOP_L_MIN) {
        this->finish_(now, EndReason::FLOW_STOP);
      }
      if (this->current_.active && this->fresh_(sample, now) && value > this->current_.max_flow_l_min)
        this->current_.max_flow_l_min = value;
    } else if (signal == Signal::POWER_KCAL_MIN && this->current_.active && this->fresh_(sample, now) &&
               !this->energy_veto_(now) && value > this->current_.max_power_kcal_min) {
      this->current_.max_power_kcal_min = value;
    }
  }

  void tick() {
    const uint32_t now = now_ms();
    this->advance_(now);
    this->emit_plot_if_due_(now);
  }

  Totals totals() const { return this->totals_; }
  Session current_session() const { return this->current_; }
  Settings settings() const { return this->settings_; }
  void configure(Settings settings) {
    if (settings.gas_type != GasType::GLP)
      settings.gas_type = GasType::GN;
    if (!valid_amount(settings.gas_tariff_per_unit))
      settings.gas_tariff_per_unit = 0;
    if (!valid_amount(settings.water_tariff_per_m3))
      settings.water_tariff_per_m3 = 0;
    this->settings_ = settings;
  }

  Estimate estimate(double water_l, double energy_kwh) const {
    Estimate result;
    if (!valid_amount(water_l) || !valid_amount(energy_kwh)) {
      result.gas_units = result.water_m3 = UNKNOWN;
      return result;
    }
    result.water_m3 = water_l / 1000.0;
    const double factor = this->settings_.gas_type == GasType::GN ? GN_KCAL_PER_M3 : GLP_KCAL_PER_KG;
    result.gas_units = energy_kwh * (3600.0 / (KJ_PER_KCAL * factor));
    result.gas_cost_valid = this->settings_.gas_tariff_per_unit > 0 && std::isfinite(result.gas_units);
    result.water_cost_valid = this->settings_.water_tariff_per_m3 > 0;
    if (result.gas_cost_valid) {
      result.gas_cost = result.gas_units * this->settings_.gas_tariff_per_unit;
      result.gas_cost_valid = std::isfinite(result.gas_cost);
      if (!result.gas_cost_valid)
        result.gas_cost = UNKNOWN;
    }
    if (result.water_cost_valid) {
      result.water_cost = result.water_m3 * this->settings_.water_tariff_per_m3;
      result.water_cost_valid = std::isfinite(result.water_cost);
      if (!result.water_cost_valid)
        result.water_cost = UNKNOWN;
    }
    result.cost_valid = result.gas_cost_valid && result.water_cost_valid;
    if (result.cost_valid) {
      result.total_cost = result.gas_cost + result.water_cost;
      result.cost_valid = std::isfinite(result.total_cost);
      if (!result.cost_valid)
        result.total_cost = UNKNOWN;
    }
    return result;
  }

  size_t point_count() const { return this->point_count_; }
  Point point(size_t oldest_index) const {
    if (oldest_index >= this->point_count_)
      return Point{};
    const size_t oldest = (this->point_next_ + POINT_CAPACITY - this->point_count_) % POINT_CAPACITY;
    return decode_point(this->points_[(oldest + oldest_index) % POINT_CAPACITY]);
  }
  size_t recent_session_count() const { return this->history_count_; }
  Session recent_session(size_t oldest_index) const {
    if (oldest_index >= this->history_count_)
      return Session{};
    const size_t oldest = (this->history_next_ + SESSION_CAPACITY - this->history_count_) % SESSION_CAPACITY;
    return this->history_[(oldest + oldest_index) % SESSION_CAPACITY];
  }
  bool take_session_finished() {
    const bool result = this->session_finished_;
    this->session_finished_ = false;
    return result;
  }
  uint32_t skipped_gaps() const { return this->skipped_gaps_; }

  Persist persist_snapshot() const {
    Persist saved{};
    saved.water_l = this->totals_.water_l;
    saved.energy_kwh = this->totals_.energy_kwh;
    saved.heating_seconds = this->totals_.heating_seconds;
    saved.sessions = this->totals_.sessions;
    saved.heating_remainder_ms = static_cast<uint16_t>(this->heat_remainder_ms_);
    return saved;
  }

  bool restore(const Persist &saved) {
    if (saved.version != 1 || !valid_amount(saved.water_l) || !valid_amount(saved.energy_kwh) ||
        saved.heating_remainder_ms >= 1000)
      return false;
    this->totals_ = Totals{saved.water_l, saved.energy_kwh, saved.heating_seconds, saved.sessions};
    this->heat_remainder_ms_ = saved.heating_remainder_ms;
    // Old telemetry, an interrupted session, and uptime history are not inferred
    // across a boot. Only supplied cumulative counters survive restoration.
    for (auto &sample : this->samples_)
      sample = Sample{};
    this->current_ = Session{};
    this->session_heat_remainder_ms_ = 0;
    this->session_finished_ = false;
    this->history_count_ = this->history_next_ = 0;
    this->point_count_ = this->point_next_ = 0;
    this->plot_window_ = CompactPoint{};
    this->plot_seen_ms_ = {};
    this->skipped_gaps_ = 0;
    this->clock_started_ = true;
    this->last_account_ms_ = this->last_point_ms_ = now_ms();
    return true;
  }

  // Explicitly rebase cumulative counters; telemetry and RAM history remain.
  // This is not called automatically and does not persist anything by itself.
  bool set_counters(const Totals &totals) {
    if (!valid_amount(totals.water_l) || !valid_amount(totals.energy_kwh))
      return false;
    this->advance_(now_ms());
    this->totals_ = totals;
    this->heat_remainder_ms_ = 0;
    if (this->current_.active)
      this->current_.sequence = saturating_add(totals.sessions, 1);
    return true;
  }

 private:
  void observe_plot_(Signal signal, const Sample &sample, uint32_t now) {
    if (!this->fresh_(sample, now)) return;
    uint16_t value = 0;
    const auto slot = static_cast<size_t>(signal);
    switch (signal) {
      case Signal::FLOW_L_MIN:
        if (!plot_quantize(sample.value, 100.0, value)) return;
        if (!(this->plot_window_.valid_mask & PLOT_FLOW) || value >= this->plot_window_.flow_centi) {
          this->plot_window_.flow_centi = value;
          this->plot_seen_ms_[slot] = sample.seen_ms;
        }
        this->plot_window_.valid_mask |= PLOT_FLOW;
        break;
      case Signal::POWER_KCAL_MIN:
        if (!plot_quantize(sample.value, 10.0, value)) return;
        if (!(this->plot_window_.valid_mask & PLOT_POWER) || value >= this->plot_window_.power_deci) {
          this->plot_window_.power_deci = value;
          this->plot_seen_ms_[slot] = sample.seen_ms;
        }
        this->plot_window_.valid_mask |= PLOT_POWER;
        break;
      case Signal::INLET_C:
        if (!plot_quantize(sample.value, 100.0, value)) return;
        this->plot_window_.inlet_centi = value;
        this->plot_window_.valid_mask |= PLOT_INLET;
        this->plot_seen_ms_[slot] = sample.seen_ms;
        break;
      case Signal::OUTLET_C:
        if (!plot_quantize(sample.value, 100.0, value)) return;
        this->plot_window_.outlet_centi = value;
        this->plot_window_.valid_mask |= PLOT_OUTLET;
        this->plot_seen_ms_[slot] = sample.seen_ms;
        break;
      case Signal::FLAME:
        if (this->plot_window_.flame < 0 || sample.value > 0.5) {
          this->plot_window_.flame = sample.value > 0.5 ? 1 : 0;
          this->plot_seen_ms_[slot] = sample.seen_ms;
        } else if (this->plot_window_.flame == 0) {
          this->plot_seen_ms_[slot] = sample.seen_ms;
        }
        break;
    }
  }

  void emit_plot_if_due_(uint32_t now) {
    if (!this->clock_started_ || static_cast<uint32_t>(now - this->last_point_ms_) < POINT_INTERVAL_MS) return;
    auto point = this->plot_window_;
    point.ms = now;
    const auto in_window = [&](Signal signal) {
      return static_cast<uint32_t>(now - this->plot_seen_ms_[static_cast<size_t>(signal)]) <= POINT_INTERVAL_MS;
    };
    if (!in_window(Signal::FLOW_L_MIN)) point.valid_mask &= static_cast<uint8_t>(~PLOT_FLOW);
    if (!in_window(Signal::POWER_KCAL_MIN)) point.valid_mask &= static_cast<uint8_t>(~PLOT_POWER);
    if (!in_window(Signal::INLET_C)) point.valid_mask &= static_cast<uint8_t>(~PLOT_INLET);
    if (!in_window(Signal::OUTLET_C)) point.valid_mask &= static_cast<uint8_t>(~PLOT_OUTLET);
    if (!in_window(Signal::FLAME)) point.flame = -1;
    this->points_[this->point_next_] = point;
    this->point_next_ = (this->point_next_ + 1) % POINT_CAPACITY;
    if (this->point_count_ < POINT_CAPACITY) ++this->point_count_;
    this->last_point_ms_ = now;
    this->plot_window_ = CompactPoint{};
    this->plot_seen_ms_ = {};
  }

  const Sample &sample_(Signal signal) const { return this->samples_[static_cast<size_t>(signal)]; }
  bool fresh_(const Sample &sample, uint32_t now) const {
    return sample.valid && static_cast<uint32_t>(now - sample.seen_ms) <= FRESH_MS;
  }
  double fresh_value_(Signal signal, uint32_t now) const {
    const Sample &sample = this->sample_(signal);
    return this->fresh_(sample, now) ? sample.value : UNKNOWN;
  }
  uint32_t eligible_ms_(const Sample &sample, uint32_t start, uint32_t elapsed) const {
    if (!sample.valid)
      return 0;
    const uint32_t age = static_cast<uint32_t>(start - sample.seen_ms);
    return age >= FRESH_MS ? 0 : smaller(elapsed, FRESH_MS - age);
  }
  bool energy_veto_(uint32_t now) const {
    const Sample &flow = this->sample_(Signal::FLOW_L_MIN);
    const Sample &flame = this->sample_(Signal::FLAME);
    return (this->fresh_(flow, now) && flow.value <= SESSION_STOP_L_MIN) ||
           (this->fresh_(flame, now) && flame.value <= 0.5);
  }
  void add_heating_(uint32_t milliseconds, uint32_t &seconds, uint32_t &remainder) {
    const uint32_t combined = remainder + milliseconds;
    seconds = saturating_add(seconds, combined / 1000);
    remainder = combined % 1000;
  }
  void finish_(uint32_t at, EndReason reason) {
    if (!this->current_.active)
      return;
    this->current_.active = false;
    this->current_.end_ms = at;
    this->current_.complete = reason == EndReason::FLOW_STOP;
    this->current_.end_reason = reason;
    this->totals_.sessions = saturating_add(this->totals_.sessions, 1);
    this->history_[this->history_next_] = this->current_;
    this->history_next_ = (this->history_next_ + 1) % SESSION_CAPACITY;
    if (this->history_count_ < SESSION_CAPACITY)
      ++this->history_count_;
    this->session_finished_ = true;
  }
  void advance_(uint32_t now) {
    if (!this->clock_started_) {
      this->clock_started_ = true;
      this->last_account_ms_ = this->last_point_ms_ = now;
      return;
    }
    const uint32_t start = this->last_account_ms_;
    const uint32_t elapsed = static_cast<uint32_t>(now - start);
    this->last_account_ms_ = now;
    if (elapsed == 0)
      return;
    if (elapsed > MAX_INTEGRATION_GAP_MS) {
      this->skipped_gaps_ = saturating_add(this->skipped_gaps_, 1);
      this->finish_(start, EndReason::INTEGRATION_GAP);
      return;
    }
    const Sample &flow = this->sample_(Signal::FLOW_L_MIN);
    const Sample &power = this->sample_(Signal::POWER_KCAL_MIN);
    const Sample &flame = this->sample_(Signal::FLAME);
    const uint32_t flow_ms = this->eligible_ms_(flow, start, elapsed);
    const uint32_t power_ms = this->eligible_ms_(power, start, elapsed);
    const uint32_t flame_ms = this->eligible_ms_(flame, start, elapsed);
    const double water_l = flow.valid ? flow.value * (static_cast<double>(flow_ms) / 60000.0) : 0;
    // A fresh stopped-flow OR extinguished-flame observation vetoes held power.
    // All sample values here were known at interval start; new samples are only
    // installed by observe() after this method completes.
    uint32_t veto_ms = 0;
    if (flow.valid && flow.value <= SESSION_STOP_L_MIN)
      veto_ms = flow_ms;
    if (flame.valid && flame.value <= 0.5)
      veto_ms = larger(veto_ms, flame_ms);
    const uint32_t energy_ms = power_ms > veto_ms ? power_ms - veto_ms : 0;
    const double power_factor = power.valid ? power.value * (KJ_PER_KCAL / (60000.0 * 3600.0)) : 0;
    const double energy_kwh = power_factor * static_cast<double>(energy_ms);
    const uint32_t heating_ms = flame.valid && flame.value > 0.5 ? flame_ms : 0;
    add_amount(this->totals_.water_l, water_l);
    add_amount(this->totals_.energy_kwh, energy_kwh);
    this->add_heating_(heating_ms, this->totals_.heating_seconds, this->heat_remainder_ms_);
    if (this->current_.active) {
      add_amount(this->current_.water_l, water_l);
      const uint32_t session_power_end = smaller(power_ms, flow_ms);
      const uint32_t session_energy_ms = session_power_end > veto_ms ? session_power_end - veto_ms : 0;
      add_amount(this->current_.energy_kwh, power_factor * static_cast<double>(session_energy_ms));
      if (session_energy_ms > 0 && power.value > this->current_.max_power_kcal_min)
        this->current_.max_power_kcal_min = power.value;
      this->current_.duration_ms = saturating_add(this->current_.duration_ms, flow_ms);
      this->add_heating_(smaller(heating_ms, flow_ms), this->current_.heating_seconds,
                         this->session_heat_remainder_ms_);
      if (!this->fresh_(flow, now))
        this->finish_(start + flow_ms, EndReason::STALE_FLOW);
    }
  }

  Totals totals_{};
  Settings settings_{};
  Session current_{};
  std::array<Sample, 5> samples_{};
  std::array<CompactPoint, POINT_CAPACITY> points_{};
  CompactPoint plot_window_{};
  std::array<uint32_t, 5> plot_seen_ms_{};
  std::array<Session, SESSION_CAPACITY> history_{};
  size_t point_count_{0};
  size_t point_next_{0};
  size_t history_count_{0};
  size_t history_next_{0};
  bool clock_started_{false};
  bool session_finished_{false};
  uint32_t last_account_ms_{0};
  uint32_t last_point_ms_{0};
  uint32_t heat_remainder_ms_{0};
  uint32_t session_heat_remainder_ms_{0};
  uint32_t skipped_gaps_{0};
};

inline Accounting accounting{};

}  // namespace detail

inline void observe(Signal signal, double value, uint32_t seen_ms) { detail::accounting.observe(signal, value, seen_ms); }
inline void tick() { detail::accounting.tick(); }
inline Totals totals() { return detail::accounting.totals(); }
inline Session current_session() { return detail::accounting.current_session(); }
inline Settings settings() { return detail::accounting.settings(); }
inline void configure(Settings settings) { detail::accounting.configure(settings); }
inline Estimate estimate(double water_l, double energy_kwh) { return detail::accounting.estimate(water_l, energy_kwh); }
inline size_t point_count() { return detail::accounting.point_count(); }
inline Point point(size_t oldest_index) { return detail::accounting.point(oldest_index); }
inline size_t recent_session_count() { return detail::accounting.recent_session_count(); }
inline Session recent_session(size_t oldest_index) { return detail::accounting.recent_session(oldest_index); }
inline bool take_session_finished() { return detail::accounting.take_session_finished(); }
inline uint32_t skipped_gaps() { return detail::accounting.skipped_gaps(); }
inline Persist persist_snapshot() { return detail::accounting.persist_snapshot(); }
inline bool restore(const Persist &saved) { return detail::accounting.restore(saved); }
inline bool set_counters(const Totals &totals) { return detail::accounting.set_counters(totals); }

}  // namespace heater_stats
