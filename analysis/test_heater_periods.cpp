#include <cstdint>
static uint32_t fake_ms = 0;
#define HEATER_STATS_MILLIS() fake_ms
#include "../esphome/heater_periods.h"
#include <cstdlib>
#include <iostream>
#include <limits>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; std::exit(EXIT_FAILURE); } } while (false)
using heater_periods::Periods;
using heater_periods::Snapshot;
using heater_stats::Totals;
static bool near(double actual, double expected, double tolerance = 1e-8) {
  return std::isfinite(actual) && std::abs(actual - expected) <= tolerance;
}
static uint64_t local_epoch(int year, unsigned month, unsigned day, unsigned hour = 0,
                            unsigned minute = 0, unsigned second = 0) {
  return static_cast<uint64_t>(heater_periods::detail::days_from_civil(year, month, day) * 86400 +
                               hour * 3600 + minute * 60 + second + 10800);
}
static Totals increment(Totals &totals, double water = 1, double energy = 0.1, uint32_t heat = 1, uint32_t sessions = 0) {
  totals.water_l += water; totals.energy_kwh += energy; totals.heating_seconds += heat; totals.sessions += sessions;
  return totals;
}

static void seed_excludes_legacy_and_normal_deltas() {
  Periods p;
  Totals totals{214.5, 4.1, 1744, 5};
  auto epoch = local_epoch(2026, 10, 26, 12);
  p.seed(totals, epoch);
  CHECK(p.today(epoch).key == 20261026);
  CHECK(p.month(epoch).key == 202610);
  CHECK(p.today(epoch).partial && p.month(epoch).partial);
  CHECK(p.tracked_since_epoch() == epoch);
  CHECK(near(p.today(epoch).water_l, 0));
  CHECK(near(p.month(epoch).energy_kwh, 0));
  CHECK(near(p.unassigned().water_l, 0));
  for (unsigned second = 1; second <= 60; ++second) p.observe(increment(totals), epoch + second);
  CHECK(near(p.today(epoch).water_l, 60));
  CHECK(near(p.today(epoch).energy_kwh, 6));
  CHECK(p.today(epoch).heating_seconds == 60);
  CHECK(p.today(epoch).sessions == 0);
  p.observe(increment(totals, 0, 0, 0, 1), epoch + 61);
  CHECK(p.today(epoch).sessions == 1);
  CHECK(near(p.month(epoch).water_l, 60));
  CHECK(p.month(epoch).sessions == 1);
  CHECK(p.day_count() == 1 && p.month_count() == 1);
  CHECK(p.day(1).key == 0 && p.month_record(1).key == 0);
  CHECK(p.today(0).key == 0 && p.month(0).key == 0);
}

static void midnight_and_month_split_conserves_totals() {
  Periods p;
  Totals totals{};
  const auto before = local_epoch(2026, 12, 31, 23, 59, 59);
  p.seed(totals, before);
  p.observe(increment(totals, 4, 0.2, 2, 1), before + 2);
  CHECK(p.day_count() == 2 && p.month_count() == 2);
  CHECK(p.day(0).key == 20261231 && p.day(1).key == 20270101);
  CHECK(p.month_record(0).key == 202612 && p.month_record(1).key == 202701);
  CHECK(near(p.day(0).water_l, 2) && near(p.day(1).water_l, 2));
  CHECK(near(p.month_record(0).energy_kwh, 0.1) && near(p.month_record(1).energy_kwh, 0.1));
  CHECK(p.day(0).heating_seconds == 1 && p.day(1).heating_seconds == 1);
  CHECK(p.day(0).sessions == 0 && p.day(1).sessions == 1);
  CHECK(p.month_record(0).sessions == 0 && p.month_record(1).sessions == 1);
  CHECK(p.day(0).partial && !p.day(1).partial);
  CHECK(p.month_record(0).partial && !p.month_record(1).partial);
  // Continuing a day/month that was seen from its start must remain complete.
  p.observe(increment(totals, 1, 0.1, 1), before + 3);
  CHECK(!p.day(1).partial && !p.month_record(1).partial);
  CHECK(near(p.day(1).water_l, 3));

  Periods odd;
  Totals other{};
  odd.seed(other, before);
  odd.observe(increment(other, 3, 0.3, 1, 0), before + 2);
  CHECK(odd.day(0).heating_seconds + odd.day(1).heating_seconds == 1);
  CHECK(near(odd.day(0).water_l + odd.day(1).water_l, 3));
  // A close event exactly at midnight belongs to the new date; volume before it
  // belongs to the previous half-open integration interval.
  Periods exact;
  Totals exact_totals{};
  exact.seed(exact_totals, before);
  exact.observe(increment(exact_totals, 2, 0.2, 1, 1), before + 1);
  CHECK(near(exact.day(0).water_l, 2) && near(exact.day(1).water_l, 0));
  CHECK(exact.day(0).sessions == 0 && exact.day(1).sessions == 1);
}

static void clock_missing_gaps_regression_and_rebases() {
  Periods p;
  Totals totals{200, 4, 1000, 5};
  const auto noon = local_epoch(2026, 10, 26, 12);
  p.seed(totals, 0);
  p.observe(increment(totals, 2, 0.2, 1, 1), 0);
  CHECK(p.day_count() == 0 && p.month_count() == 0);
  CHECK(near(p.unassigned().water_l, 2) && p.unassigned().sessions == 1);
  p.observe(increment(totals, 3, 0.3, 1), noon);
  CHECK(near(p.unassigned().water_l, 5));
  CHECK(near(p.today(noon).water_l, 0));
  CHECK(p.tracked_since_epoch() == noon);
  p.observe(increment(totals, 1, 0.1, 1), noon + 1);
  CHECK(near(p.today(noon).water_l, 1));
  p.observe(increment(totals, 10, 1, 10, 1), noon + 40);
  CHECK(near(p.unassigned().water_l, 15));
  CHECK(near(p.today(noon).water_l, 1));
  p.observe(increment(totals, 1, 0.1, 1), noon + 41);
  CHECK(near(p.today(noon).water_l, 2));
  // A clock regression must not reassign subsequent deltas to old dates.
  p.observe(increment(totals, 2, 0.2, 1), noon - 86400);
  p.observe(increment(totals, 2, 0.2, 1), noon - 86399);
  CHECK(near(p.unassigned().water_l, 19));
  CHECK(near(p.today(noon).water_l, 2));
  p.observe(increment(totals, 1, 0.1, 1), noon + 42);
  CHECK(near(p.unassigned().water_l, 20));
  p.observe(increment(totals, 1, 0.1, 1), noon + 43);
  CHECK(near(p.today(noon).water_l, 3));
  // Explicit counter rebases establish a new baseline, never negative usage.
  totals = Totals{0, 0, 0, 0};
  p.observe(totals, noon + 44);
  p.observe(increment(totals, 4, 0.4, 1, 1), noon + 45);
  CHECK(near(p.today(noon).water_l, 7));
  CHECK(p.today(noon).sessions == 1);
  const auto saved_total = p.snapshot();
  Totals invalid = totals; invalid.water_l = std::numeric_limits<double>::quiet_NaN();
  p.observe(invalid, noon + 46);
  p.observe(increment(totals, 3, 0.3, 1), noon + 47);
  CHECK(near(p.today(noon).water_l, 7));
  CHECK(near(p.unassigned().water_l, saved_total.unassigned.water_l + 3));
}

static void restore_seed_preserves_rows_and_excludes_reboot_gap() {
  Periods p;
  Totals totals{};
  const auto start = local_epoch(2027, 1, 1);
  p.seed(totals, start);
  p.observe(increment(totals, 4, 0.4, 1, 1), start + 1);
  CHECK(!p.today(start).partial && !p.month(start).partial);
  const auto saved = p.snapshot();
  CHECK(saved.version == 1 && saved.offset_seconds == -10800);
  CHECK(saved.day_count == 1 && saved.month_count == 1);
  Periods restored;
  CHECK(restored.restore(saved));
  CHECK(near(restored.today(start).water_l, 4));
  // Freshly supplied cumulative values may include an unobserved interval;
  // seed is a baseline, not a request to allocate the difference to today.
  increment(totals, 50, 5, 100, 2);
  restored.seed(totals, start + 120);
  CHECK(near(restored.today(start).water_l, 4));
  CHECK(restored.today(start).partial && restored.month(start).partial);
  restored.observe(increment(totals, 2, 0.2, 1, 1), start + 121);
  CHECK(near(restored.today(start).water_l, 6));
  CHECK(restored.today(start).sessions == 2);
  CHECK(restored.tracked_since_epoch() == start);
  CHECK(near(restored.unassigned().water_l, 0));
  CHECK(restored.snapshot().days[1].key == 0);
}

static void date_boundaries_and_ring_overflow() {
  CHECK(!heater_periods::detail::leap(2100));
  CHECK(heater_periods::detail::leap(2000));
  CHECK(heater_periods::detail::month_days(2100, 2) == 28);
  CHECK(heater_periods::detail::month_days(2028, 2) == 29);
  CHECK(heater_periods::detail::month_days(2028, 13) == 0);
  Periods leap;
  Totals totals{};
  const auto leap_before = local_epoch(2100, 2, 28, 23, 59, 59);
  leap.seed(totals, leap_before);
  leap.observe(increment(totals, 2, 0.2, 2), leap_before + 2);
  CHECK(leap.day(0).key == 21000228 && leap.day(1).key == 21000301);
  const auto local_midnight = local_epoch(2026, 10, 27);
  Periods zone;
  zone.seed(Totals{}, local_midnight - 1);
  CHECK(zone.today(local_midnight - 1).key == 20261026);
  CHECK(zone.today(local_midnight).key == 20261027);

  Periods ring;
  Totals empty{};
  // Long gaps produce zero-valued partial periods, never filled-in usage.
  for (unsigned month = 1; month <= 20; ++month) {
    const int year = 2026 + static_cast<int>((month - 1) / 12);
    const unsigned date_month = (month - 1) % 12 + 1;
    const auto epoch = local_epoch(year, date_month, 1);
    if (month == 1) ring.seed(empty, epoch); else ring.observe(empty, epoch);
    for (unsigned day = 2; day <= 28; ++day) ring.observe(empty, local_epoch(year, date_month, day));
  }
  CHECK(ring.day_count() == 31 && ring.month_count() == 12);
  CHECK(ring.month_record(0).key == 202609 && ring.month_record(11).key == 202708);
  CHECK(ring.day(30).key == 20270828);
  auto saved = ring.snapshot();
  CHECK(saved.days[0].key < saved.days[30].key);
  for (size_t i = 1; i < saved.day_count; ++i) CHECK(saved.days[i - 1].key < saved.days[i].key);
  for (size_t i = 1; i < saved.month_count; ++i) CHECK(saved.months[i - 1].key < saved.months[i].key);
  Periods reboot;
  CHECK(reboot.restore(saved));
  CHECK(reboot.day_count() == 31 && reboot.month_count() == 12);
}

static void corrupted_snapshots_are_rejected_transactionally() {
  Periods p;
  Totals totals{};
  const auto now = local_epoch(2026, 10, 26, 12);
  p.seed(totals, now);
  p.observe(increment(totals), now + 1);
  const auto good = p.snapshot();
  Periods target;
  CHECK(target.restore(good));
  auto verify_rejected = [&](Snapshot bad) {
    CHECK(!target.restore(bad));
    CHECK(near(target.today(now).water_l, good.days[0].water_l));
    CHECK(target.day_count() == good.day_count);
  };
  auto bad = good; bad.version = 2; verify_rejected(bad);
  bad = good; bad.reserved = 1; verify_rejected(bad);
  bad = good; bad.offset_seconds = 86401; verify_rejected(bad);
  bad = good; bad.tracked_since_epoch = 10; verify_rejected(bad);
  bad = good; bad.day_count = 32; verify_rejected(bad);
  bad = good; bad.month_count = 13; verify_rejected(bad);
  bad = good; bad.days[0].key = 21000229; verify_rejected(bad);
  bad = good; bad.months[0].key = 202613; verify_rejected(bad);
  bad = good; bad.days[0].water_l = -1; verify_rejected(bad);
  bad = good; bad.months[0].energy_kwh = std::numeric_limits<double>::infinity(); verify_rejected(bad);
  bad = good; bad.days[0].partial = 2; verify_rejected(bad);
  bad = good; bad.days[0].reserved[0] = 1; verify_rejected(bad);
  bad = good; bad.days[1].key = 20261027; verify_rejected(bad);
  bad = good; bad.unassigned.water_l = std::numeric_limits<double>::quiet_NaN(); verify_rejected(bad);
  bad = good; bad.days[1] = good.days[0]; bad.day_count = 2; verify_rejected(bad);
  CHECK(heater_periods::clock_valid(now));
  CHECK(!heater_periods::clock_valid(0));
  CHECK(!heater_periods::clock_valid(UINT64_MAX));
}

int main() {
  seed_excludes_legacy_and_normal_deltas();
  midnight_and_month_split_conserves_totals();
  clock_missing_gaps_regression_and_rebases();
  restore_seed_preserves_rows_and_excludes_reboot_gap();
  date_boundaries_and_ring_overflow();
  corrupted_snapshots_are_rejected_transactionally();
  std::cout << "PASS: " << checks << " checks across 6 period-summary scenarios\n";
}
