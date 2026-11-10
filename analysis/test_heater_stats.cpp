// Deterministic host accounting tests. No ESP-IDF, NVS or network dependency.
#include <cstdint>
static uint32_t fake_ms = 0;
#define HEATER_STATS_MILLIS() fake_ms
#include "../esphome/heater_stats.h"

#include <cstdlib>
#include <iostream>

using Accounting = heater_stats::detail::Accounting;
using heater_stats::Signal;
using heater_stats::GasType;
using heater_stats::Settings;
using heater_stats::EndReason;
static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; \
  std::exit(EXIT_FAILURE); } } while (false)
static bool near(double a, double b, double tolerance = 1e-9) {
  return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
}

static void observe(Accounting &a, Signal signal, double value) {
  a.observe(signal, value, fake_ms);
}
static void seed(Accounting &a, double flow = 10, double power = 1000, double flame = 1) {
  observe(a, Signal::FLOW_L_MIN, flow);
  observe(a, Signal::POWER_KCAL_MIN, power);
  observe(a, Signal::FLAME, flame);
  observe(a, Signal::INLET_C, 20);
  observe(a, Signal::OUTLET_C, 38);
}
static void minute(Accounting &a) {
  seed(a);
  for (unsigned second = 1; second <= 60; ++second) {
    fake_ms += 1000;
    // Hold each observation for <=10 s, with the final refresh at second50.
    if (second % 10 == 0 && second < 60)
      seed(a);
    a.tick();
  }
}

static void minute_oracle_and_costs() {
  Accounting a;
  fake_ms = 1000;
  minute(a);
  const auto totals = a.totals();
  CHECK(near(totals.water_l, 10));
  CHECK(near(totals.energy_kwh, 1.163));
  CHECK(totals.heating_seconds == 60);
  CHECK(a.skipped_gaps() == 0);
  CHECK(a.current_session().active);
  CHECK(a.current_session().duration_ms == 60000);
  CHECK(near(a.current_session().water_l, 10));
  CHECK(near(a.current_session().energy_kwh, 1.163));
  CHECK(near(a.current_session().max_flow_l_min, 10));
  CHECK(near(a.current_session().max_power_kcal_min, 1000));

  auto cost = a.estimate(totals.water_l, totals.energy_kwh);
  CHECK(near(cost.gas_units, 1000.0 / 9400.0));
  CHECK(near(cost.water_m3, 0.01));
  CHECK(!cost.cost_valid);
  CHECK(std::isnan(cost.gas_cost));
  CHECK(std::isnan(cost.water_cost));
  CHECK(std::isnan(cost.total_cost));
  a.configure(Settings{GasType::GN, 8, 10});
  cost = a.estimate(totals.water_l, totals.energy_kwh);
  CHECK(cost.cost_valid);
  CHECK(near(cost.gas_cost, (1000.0 / 9400.0) * 8));
  CHECK(near(cost.water_cost, 0.1));
  CHECK(near(cost.total_cost, 0.951063829787234));
  a.configure(Settings{GasType::GLP, 8, 10});
  CHECK(near(a.estimate(10, 1.163).gas_units, 1000.0 / 11500.0));
  a.configure(Settings{GasType::GN, 8, 0});
  cost = a.estimate(10, 1.163);
  CHECK(cost.gas_cost_valid);
  CHECK(!cost.water_cost_valid);
  CHECK(!cost.cost_valid);
  CHECK(std::isnan(cost.total_cost));
  a.configure(Settings{GasType::GN, heater_stats::UNKNOWN, -1});
  CHECK(a.settings().gas_tariff_per_unit == 0);
  CHECK(a.settings().water_tariff_per_m3 == 0);
  CHECK(std::isnan(a.estimate(-1, 1).gas_units));
  CHECK(std::isnan(a.estimate(1, heater_stats::UNKNOWN).water_m3));

  observe(a, Signal::FLOW_L_MIN, 0);
  CHECK(!a.current_session().active);
  CHECK(a.current_session().complete);
  CHECK(a.current_session().end_reason == EndReason::FLOW_STOP);
  CHECK(a.totals().sessions == 1);
  CHECK(a.recent_session_count() == 1);
  CHECK(a.take_session_finished());
  CHECK(!a.take_session_finished());
  std::cout << "ORACLE: 10 L; 1.163 kWh; GN=" << 1000.0 / 9400.0
            << " m3; total R$=" << 0.951063829787234 << " (gas8/water10)\n";
}

static void no_backdating_or_old_records() {
  Accounting a;
  fake_ms = 1000;
  seed(a, 0, 0, 0);
  fake_ms = 2000;
  seed(a);
  CHECK(near(a.totals().water_l, 0));
  CHECK(near(a.totals().energy_kwh, 0));
  fake_ms = 3000;
  a.tick();
  CHECK(near(a.totals().water_l, 10.0 / 60.0));
  CHECK(near(a.totals().energy_kwh, 1.163 / 60.0));
  a.observe(Signal::FLOW_L_MIN, 999, 2000);  // Duplicate.
  a.observe(Signal::FLOW_L_MIN, 999, 1999);  // Older.
  a.observe(Signal::FLOW_L_MIN, 999, 3001);  // Future.
  fake_ms = 4000;
  a.tick();
  CHECK(near(a.totals().water_l, 20.0 / 60.0));
  CHECK(near(a.current_session().max_flow_l_min, 10));
}

static void flow_zero_and_flame_off_veto_energy() {
  for (unsigned variant = 0; variant < 2; ++variant) {
    Accounting a;
    fake_ms = 1000;
    seed(a, variant == 0 ? 0 : 10, 1000, variant == 0 ? 1 : 0);
    for (unsigned second = 0; second < 5; ++second) {
      fake_ms += 1000;
      a.tick();
    }
    CHECK(near(a.totals().energy_kwh, 0));
    CHECK(near(a.totals().water_l, variant == 0 ? 0 : 50.0 / 60.0));
  }
  Accounting transition;
  fake_ms = 1000;
  seed(transition);
  fake_ms = 2000;
  observe(transition, Signal::FLAME, 0);
  CHECK(near(transition.totals().energy_kwh, 1.163 / 60.0));
  fake_ms = 3000;
  transition.tick();
  CHECK(near(transition.totals().energy_kwh, 1.163 / 60.0));
  CHECK(near(transition.totals().water_l, 20.0 / 60.0));
}

static void gap_and_staleness_boundaries() {
  Accounting gap;
  fake_ms = 1000;
  seed(gap);
  fake_ms = 2000;
  gap.tick();
  const auto before = gap.totals();
  fake_ms = 7001;
  gap.tick();
  CHECK(near(gap.totals().water_l, before.water_l));
  CHECK(near(gap.totals().energy_kwh, before.energy_kwh));
  CHECK(gap.skipped_gaps() == 1);
  CHECK(!gap.current_session().active);
  CHECK(!gap.current_session().complete);
  CHECK(gap.current_session().end_reason == EndReason::INTEGRATION_GAP);
  CHECK(gap.current_session().end_ms == 2000);

  Accounting exact_gap;
  fake_ms = 1000;
  seed(exact_gap);
  fake_ms = 6000;
  exact_gap.tick();
  CHECK(exact_gap.skipped_gaps() == 0);
  CHECK(near(exact_gap.totals().water_l, 50.0 / 60.0));

  Accounting stale;
  fake_ms = 1000;
  seed(stale);
  for (unsigned second = 1; second <= 240; ++second) {
    fake_ms += 1000;
    stale.tick();
  }
  CHECK(near(stale.totals().water_l, 5));
  CHECK(near(stale.totals().energy_kwh, 0.5815));
  CHECK(stale.totals().heating_seconds == 30);
  CHECK(stale.current_session().duration_ms == 30000);
  CHECK(!stale.current_session().complete);
  CHECK(stale.current_session().end_reason == EndReason::STALE_FLOW);
  CHECK(stale.current_session().end_ms == 31000);
  CHECK(stale.point_count() == 2);
  CHECK(near(stale.point(0).flow_l_min, 10));
  CHECK(std::isnan(stale.point(1).flow_l_min));
  CHECK(std::isnan(stale.point(1).power_kcal_min));
  CHECK(std::isnan(stale.point(1).inlet_c));
  CHECK(stale.point(1).flame == -1);
}

static void restore_only_cumulative_counters() {
  Accounting original;
  fake_ms = 1000;
  seed(original);
  fake_ms = 2500;
  original.tick();
  const auto saved = original.persist_snapshot();
  CHECK(saved.heating_seconds == 1);
  CHECK(saved.heating_remainder_ms == 500);
  CHECK(saved.reserved == 0);
  Accounting restored;
  fake_ms = 500000;
  CHECK(restored.restore(saved));
  CHECK(!restored.current_session().active);
  CHECK(restored.recent_session_count() == 0);
  CHECK(restored.point_count() == 0);
  fake_ms += 1000;
  restored.tick();
  CHECK(near(restored.totals().water_l, saved.water_l));
  CHECK(near(restored.totals().energy_kwh, saved.energy_kwh));
  CHECK(restored.totals().heating_seconds == 1);
  observe(restored, Signal::FLAME, 1);
  fake_ms += 500;
  restored.tick();
  CHECK(restored.totals().heating_seconds == 2);
  CHECK(near(restored.totals().water_l, saved.water_l));
  auto bad = saved;
  bad.version = 2;
  CHECK(!restored.restore(bad));
  bad = saved;
  bad.water_l = -1;
  CHECK(!restored.restore(bad));
  bad = saved;
  bad.energy_kwh = heater_stats::UNKNOWN;
  CHECK(!restored.restore(bad));
  bad = saved;
  bad.heating_remainder_ms = 1000;
  CHECK(!restored.restore(bad));
}

static void history_and_plot_ring_buffers() {
  Accounting sessions;
  fake_ms = 1000;
  sessions.tick();
  for (unsigned session = 0; session < heater_stats::SESSION_CAPACITY + 2; ++session) {
    seed(sessions);
    fake_ms += 1000;
    sessions.tick();
    observe(sessions, Signal::FLOW_L_MIN, 0);
    fake_ms += 1000;
    sessions.tick();
  }
  CHECK(sessions.totals().sessions == heater_stats::SESSION_CAPACITY + 2);
  CHECK(sessions.recent_session_count() == heater_stats::SESSION_CAPACITY);
  CHECK(sessions.recent_session(0).sequence == 3);
  CHECK(sessions.recent_session(heater_stats::SESSION_CAPACITY - 1).sequence == heater_stats::SESSION_CAPACITY + 2);
  CHECK(sessions.recent_session(0).duration_ms == 1000);
  CHECK(near(sessions.recent_session(0).water_l, 10.0 / 60.0));
  CHECK(sessions.recent_session(0).complete);
  CHECK(sessions.recent_session(heater_stats::SESSION_CAPACITY).sequence == 0);

  Accounting points;
  fake_ms = 1000;
  points.tick();
  for (unsigned i = 1; i <= heater_stats::POINT_CAPACITY + 3; ++i) {
    fake_ms += heater_stats::POINT_INTERVAL_MS;
    points.tick();
  }
  CHECK(points.point_count() == heater_stats::POINT_CAPACITY);
  CHECK(points.point(0).ms == 1000 + 4 * heater_stats::POINT_INTERVAL_MS);
  CHECK(points.point(heater_stats::POINT_CAPACITY - 1).ms == 1000 +
        (heater_stats::POINT_CAPACITY + 3) * heater_stats::POINT_INTERVAL_MS);
  CHECK(std::isnan(points.point(0).flow_l_min));
  CHECK(points.point(0).flame == -1);
  CHECK(points.point(heater_stats::POINT_CAPACITY).ms == 0);
}

static void compact_plot_bounds_and_quantization() {
  CHECK(sizeof(heater_stats::detail::CompactPoint) == 16);
  CHECK(sizeof(std::array<heater_stats::detail::CompactPoint, heater_stats::POINT_CAPACITY>) == 11520);
  CHECK(heater_stats::POINT_CAPACITY * heater_stats::POINT_INTERVAL_MS == 86400000U);
  uint16_t raw = 123;
  CHECK(heater_stats::detail::plot_quantize(655.35, 100, raw) && raw == 65535);
  CHECK(heater_stats::detail::plot_quantize(6553.5, 10, raw) && raw == 65535);
  CHECK(heater_stats::detail::plot_quantize(0, 100, raw) && raw == 0);
  CHECK(!heater_stats::detail::plot_quantize(655.351, 100, raw));
  CHECK(!heater_stats::detail::plot_quantize(6553.51, 10, raw));
  CHECK(!heater_stats::detail::plot_quantize(-0.001, 100, raw));
  CHECK(!heater_stats::detail::plot_quantize(heater_stats::UNKNOWN, 100, raw));
  CHECK(!heater_stats::detail::plot_quantize(std::numeric_limits<double>::infinity(), 100, raw));

  Accounting a;
  fake_ms = 1000;
  observe(a, Signal::FLOW_L_MIN, 655.35);
  observe(a, Signal::POWER_KCAL_MIN, 6553.5);
  observe(a, Signal::INLET_C, 18.239);
  observe(a, Signal::OUTLET_C, 39.999);
  observe(a, Signal::FLAME, 0);
  fake_ms += heater_stats::POINT_INTERVAL_MS;
  a.tick();
  CHECK(a.point_count() == 1);
  CHECK(near(a.point(0).flow_l_min, 655.35));
  CHECK(near(a.point(0).power_kcal_min, 6553.5));
  CHECK(near(a.point(0).inlet_c, 18.24));
  CHECK(near(a.point(0).outlet_c, 40));
  CHECK(a.point(0).flame == 0);
  fake_ms += 1000;
  observe(a, Signal::FLOW_L_MIN, 655.36);
  observe(a, Signal::POWER_KCAL_MIN, 6553.6);
  observe(a, Signal::INLET_C, -1);
  observe(a, Signal::OUTLET_C, heater_stats::UNKNOWN);
  fake_ms += heater_stats::POINT_INTERVAL_MS - 1000;
  a.tick();
  CHECK(a.point_count() == 2);
  CHECK(std::isnan(a.point(1).flow_l_min));
  CHECK(std::isnan(a.point(1).power_kcal_min));
  CHECK(std::isnan(a.point(1).inlet_c));
  CHECK(std::isnan(a.point(1).outlet_c));
  CHECK(a.point(1).flame == -1);
}

static void short_tap_remains_visible_in_two_minute_plot() {
  Accounting a;
  fake_ms = 1000;
  seed(a, 0, 0, 0);
  for (unsigned second = 1; second <= 120; ++second) {
    fake_ms += 1000;
    if (second == 10) seed(a, 4.71, 82.16, 1);
    if (second == 20 || (second > 20 && second % 8 == 0)) {
      observe(a, Signal::FLOW_L_MIN, 0);
      observe(a, Signal::POWER_KCAL_MIN, 0);
      observe(a, Signal::FLAME, 0);
    }
    if (second == 119) {
      observe(a, Signal::INLET_C, 18.239);
      observe(a, Signal::OUTLET_C, 39.999);
    }
    a.tick();
  }
  CHECK(a.totals().sessions == 1);
  CHECK(a.current_session().complete);
  CHECK(a.current_session().duration_ms == 10000);
  CHECK(a.point_count() == 1);
  CHECK(near(a.point(0).flow_l_min, 4.71));
  CHECK(near(a.point(0).power_kcal_min, 82.2));
  CHECK(a.point(0).flame == 1);
  CHECK(near(a.point(0).inlet_c, 18.24));
  CHECK(near(a.point(0).outlet_c, 40));
  for (unsigned second = 1; second <= 120; ++second) {
    fake_ms += 1000;
    if (second % 8 == 0) {
      observe(a, Signal::FLOW_L_MIN, 0);
      observe(a, Signal::POWER_KCAL_MIN, 0);
      observe(a, Signal::FLAME, 0);
    }
    a.tick();
  }
  CHECK(a.point_count() == 2);
  CHECK(near(a.point(1).flow_l_min, 0));
  CHECK(near(a.point(1).power_kcal_min, 0));
  CHECK(a.point(1).flame == 0);
  CHECK(std::isnan(a.point(1).inlet_c));
  CHECK(std::isnan(a.point(1).outlet_c));
}

static void plot_gap_no_backfill_restore_and_clock_wrap() {
  Accounting a;
  fake_ms = 1000;
  seed(a);
  fake_ms = 1000000;
  a.tick();
  CHECK(a.point_count() == 1);
  CHECK(a.point(0).ms == fake_ms);
  CHECK(std::isnan(a.point(0).flow_l_min));
  CHECK(std::isnan(a.point(0).inlet_c));
  CHECK(a.point(0).flame == -1);
  const auto saved = a.persist_snapshot();
  CHECK(a.restore(saved));
  CHECK(a.point_count() == 0);
  fake_ms += heater_stats::POINT_INTERVAL_MS;
  a.tick();
  CHECK(a.point_count() == 1);
  CHECK(std::isnan(a.point(0).flow_l_min));

  Accounting wrapped;
  fake_ms = UINT32_MAX - 500;
  seed(wrapped, 4.71, 82.1, 1);
  fake_ms += heater_stats::POINT_INTERVAL_MS;
  wrapped.tick();
  CHECK(wrapped.point_count() == 1);
  CHECK(wrapped.point(0).ms == fake_ms);
  CHECK(near(wrapped.point(0).flow_l_min, 4.71));
  CHECK(wrapped.point(0).flame == 1);
}

static void clock_wrap() {
  Accounting a;
  fake_ms = UINT32_MAX - 500;
  minute(a);
  CHECK(near(a.totals().water_l, 10));
  CHECK(near(a.totals().energy_kwh, 1.163));
  CHECK(a.totals().heating_seconds == 60);
  CHECK(a.skipped_gaps() == 0);
  CHECK(a.current_session().duration_ms == 60000);
}

int main() {
  minute_oracle_and_costs();
  no_backdating_or_old_records();
  flow_zero_and_flame_off_veto_energy();
  gap_and_staleness_boundaries();
  restore_only_cumulative_counters();
  history_and_plot_ring_buffers();
  compact_plot_bounds_and_quantization();
  short_tap_remains_visible_in_two_minute_plot();
  plot_gap_no_backfill_restore_and_clock_wrap();
  clock_wrap();
  std::cout << "PASS: " << checks << " checks across 10 heater stats scenarios\n";
}
