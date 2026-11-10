#include <cstdint>
static uint32_t fake_ms = 0;
#define HEATER_STATS_MILLIS() fake_ms
#include "../esphome/heater_history.h"

#include <cstdlib>
#include <iostream>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; \
  std::exit(EXIT_FAILURE); } } while (false)
using heater_history::History;
using heater_history::Snapshot;
using heater_stats::EndReason;
static constexpr uint64_t EPOCH = 1793000000ULL;

static heater_stats::Session session(uint32_t sequence = 1) {
  heater_stats::Session result;
  result.sequence = sequence;
  result.start_ms = 10000;
  result.end_ms = 70000;
  result.duration_ms = 60000;
  result.heating_seconds = 52;
  result.water_l = 7.2;
  result.energy_kwh = 0.14;
  result.active = false;
  result.complete = true;
  result.end_reason = EndReason::FLOW_STOP;
  return result;
}

static void unknown_clock_restore_is_honest() {
  History history;
  CHECK(history.count() == 0);
  CHECK(std::isnan(history.age_s(0, 72000)));
  CHECK(history.add(session(), 72000));
  CHECK(history.count() == 1);
  CHECK(history.record(0).end_epoch_s == 0);
  CHECK(history.age_s(0, 73500) == 3.5);
  CHECK(!history.synchronize_clock(73500, 0));
  Snapshot saved = history.snapshot();
  CHECK(saved.version == 2 && saved.count == 1);
  CHECK(saved.records[1].sequence == 0);
  History restored;
  CHECK(restored.restore(saved));
  CHECK(restored.record(0).sequence == 1);
  CHECK(restored.record(0).duration_ms == 60000);
  CHECK(restored.record(0).water_l == 7.2);
  CHECK(std::isnan(restored.age_s(0, 73500)));
  CHECK(std::isnan(restored.age_s(0, 73500, EPOCH)));
  CHECK(!restored.synchronize_clock(73500, EPOCH));
  CHECK(restored.record(0).end_epoch_s == 0);
  CHECK(!history.add(session(), 74000));
  CHECK(history.count() == 1);
}

static void epochs_survive_boot_and_delayed_sync() {
  History history;
  CHECK(history.add(session(), 73000, EPOCH));
  CHECK(history.record(0).end_epoch_s == EPOCH - 3);
  CHECK(history.record(0).start_epoch_s == EPOCH - 63);
  CHECK(history.age_s(0, 76000, EPOCH + 3) == 6);
  History restored;
  CHECK(restored.restore(history.snapshot()));
  CHECK(restored.age_s(0, 5000, EPOCH + 3600) == 3603);
  CHECK(std::isnan(restored.age_s(0, 5000)));
  CHECK(std::isnan(restored.age_s(0, 5000, EPOCH - 10)));
  // A backward wall clock may still use same-boot monotonic timing.
  CHECK(history.age_s(0, 76000, EPOCH - 10) == 6);

  History delayed;
  CHECK(delayed.add(session(), 72000));
  CHECK(delayed.synchronize_clock(90000, EPOCH));
  CHECK(delayed.record(0).end_epoch_s == EPOCH - 20);
  CHECK(delayed.record(0).start_epoch_s == EPOCH - 80);
  CHECK(!delayed.synchronize_clock(100000, EPOCH + 10));
  CHECK(delayed.age_s(0, 100000, EPOCH + 10) == 30);
  CHECK(restored.restore(delayed.snapshot()));
  CHECK(restored.age_s(0, 1000, EPOCH + 100) == 120);
}

static void ring_order_and_snapshot_overflow() {
  History history;
  for (uint32_t id = 1; id <= 32; ++id) {
    CHECK(history.add(session(id), 72000 + id * 1000, EPOCH + id));
    CHECK(history.count() == (id < 24 ? id : 24));
  }
  CHECK(history.record(0).sequence == 9);
  CHECK(history.record(23).sequence == 32);
  Snapshot saved = history.snapshot();
  for (uint32_t i = 0; i < 24; ++i)
    CHECK(saved.records[i].sequence == i + 9);
  History restored;
  CHECK(restored.restore(saved));
  for (uint32_t i = 0; i < 24; ++i)
    CHECK(restored.record(i).sequence == i + 9);
  CHECK(restored.add(session(33), 110000, EPOCH + 40));
  CHECK(restored.count() == 24);
  CHECK(restored.record(0).sequence == 10);
  CHECK(restored.record(23).sequence == 33);
  CHECK(!restored.add(session(9), 100000, EPOCH + 30));
  CHECK(restored.record(24).sequence == 0);
  CHECK(std::isnan(restored.age_s(24, 110000, EPOCH + 40)));
}

static void reject_corrupt_snapshot_transactionally() {
  History history;
  CHECK(history.add(session(), 72000, EPOCH));
  const auto valid = history.snapshot();
  auto rejected = [&](Snapshot changed) {
    CHECK(!history.restore(changed));
    CHECK(history.count() == 1);
    CHECK(history.record(0).sequence == 1);
  };
  auto changed = valid; changed.version = 3; rejected(changed);
  changed = valid; changed.count = 25; rejected(changed);
  changed = valid; changed.reserved = 1; rejected(changed);
  changed = valid; changed.records[0].water_l = NAN; rejected(changed);
  changed = valid; changed.records[0].water_l = -1; rejected(changed);
  changed = valid; changed.records[0].energy_kwh = INFINITY; rejected(changed);
  changed = valid; changed.records[0].energy_kwh = -0.01; rejected(changed);
  changed = valid; changed.records[0].end_epoch_s = 123; rejected(changed);
  changed = valid; changed.records[0].start_epoch_s = 123; rejected(changed);
  changed = valid; changed.records[0].start_epoch_s = changed.records[0].end_epoch_s + 1; rejected(changed);
  changed = valid; changed.records[0].sequence = 0; rejected(changed);
  changed = valid; changed.records[0].heating_seconds = 61; rejected(changed);
  changed = valid; changed.records[0].complete = 2; rejected(changed);
  changed = valid; changed.records[0].end_reason = 4; rejected(changed);
  changed = valid; changed.records[0].end_reason = 0; rejected(changed);
  changed = valid; changed.records[0].complete = 0; rejected(changed);
  changed = valid; changed.records[0].reserved[0] = 1; rejected(changed);
  changed = valid; changed.records[1].sequence = 99; rejected(changed);
  changed = valid; changed.count = 2; changed.records[1] = changed.records[0]; rejected(changed);
  CHECK(history.restore(Snapshot{}));
  CHECK(history.count() == 0);
}

static void migrate_twelve_records_and_export_rollback() {
  History original;
  for (uint32_t id = 1; id <= 12; ++id) CHECK(original.add(session(id), 80000 + id * 1000, EPOCH + id));
  const auto old = original.legacy_snapshot();
  CHECK(sizeof(old) == 488 && old.version == 1 && old.count == 12);
  History migrated;
  CHECK(migrated.restore(old));
  CHECK(migrated.count() == 12 && migrated.snapshot().version == 2);
  for (uint32_t i = 0; i < 12; ++i) {
    const auto restored = migrated.record(i);
    CHECK(restored.sequence == old.records[i].sequence && restored.end_epoch_s == old.records[i].end_epoch_s);
    CHECK(restored.water_l == old.records[i].water_l && restored.energy_kwh == old.records[i].energy_kwh);
    CHECK(restored.start_epoch_s == 0);  // Never infer a start for an older record.
    CHECK(std::isnan(migrated.age_s(i, 1000)));
  }
  for (uint32_t id = 13; id <= 30; ++id) CHECK(migrated.add(session(id), 80000 + id * 1000, EPOCH + id));
  CHECK(migrated.count() == 24 && migrated.record(0).sequence == 7);
  const auto rollback = migrated.legacy_snapshot();
  CHECK(rollback.version == 1 && rollback.count == 12 && rollback.records[0].sequence == 19);
  CHECK(rollback.records[11].sequence == 30);
  History downgraded;
  CHECK(downgraded.restore(rollback));
  CHECK(downgraded.count() == 12 && downgraded.record(0).sequence == 19);

  const auto before = migrated.snapshot();
  auto reject_old = [&](heater_history::LegacySnapshot bad) {
    CHECK(!migrated.restore(bad));
    const auto after = migrated.snapshot();
    CHECK(std::memcmp(&before, &after, sizeof(before)) == 0);
  };
  auto bad = old; bad.version = 2; reject_old(bad);
  bad = old; bad.count = 13; reject_old(bad);
  bad = old; bad.reserved = 1; reject_old(bad);
  bad = old; bad.records[0].water_l = NAN; reject_old(bad);
  bad = old; bad.records[1].sequence = bad.records[0].sequence; reject_old(bad);
  bad = old; bad.count = 11; reject_old(bad);  // Nonzero unused old row is corrupt.
  heater_history::LegacySnapshot unknown{};
  unknown.count = 1; unknown.records[0] = old.records[0]; unknown.records[0].end_epoch_s = 0;
  CHECK(downgraded.restore(unknown));
  CHECK(std::isnan(downgraded.age_s(0, 1000, EPOCH + 3600)));
  CHECK(!downgraded.synchronize_clock(1000, EPOCH + 3600));
  CHECK(downgraded.record(0).start_epoch_s == 0);
}

static void invalid_sessions_and_uptime_wrap() {
  History history;
  auto bad = session(); bad.active = true; CHECK(!history.add(bad, 72000));
  bad = session(); bad.end_reason = EndReason::NONE; CHECK(!history.add(bad, 72000));
  bad = session(); bad.water_l = NAN; CHECK(!history.add(bad, 72000));
  bad = session(); CHECK(!history.add(bad, bad.end_ms - 1));
  auto incomplete = session(); incomplete.complete = false; incomplete.end_reason = EndReason::STALE_FLOW;
  CHECK(history.add(incomplete, 72000));
  CHECK(history.record(0).complete == 0);
  CHECK(history.record(0).end_reason == static_cast<uint8_t>(EndReason::STALE_FLOW));
  auto wrapped = session(2); wrapped.end_ms = UINT32_MAX - 999; wrapped.start_ms = wrapped.end_ms - 60000;
  CHECK(history.add(wrapped, 1000, EPOCH));
  CHECK(history.age_s(1, 2000) == 3);
  CHECK(history.record(1).end_epoch_s == EPOCH - 2);
  CHECK(history.record(1).start_epoch_s == EPOCH - 62);
  CHECK(history.record(0).end_epoch_s == 0);
  CHECK(history.synchronize_clock(100000, EPOCH + 100));
  CHECK(history.record(0).end_epoch_s == EPOCH + 70);
}

static void actual_start_is_not_reconstructed_from_accounted_duration() {
  History history;
  auto partial = session();
  partial.duration_ms = 45000;
  partial.heating_seconds = 40;
  partial.complete = false;
  partial.end_reason = EndReason::INTEGRATION_GAP;
  CHECK(history.add(partial, 73000, EPOCH));
  const auto saved = history.record(0);
  CHECK(saved.start_epoch_s == EPOCH - 63 && saved.end_epoch_s == EPOCH - 3);
  CHECK(saved.end_epoch_s - saved.start_epoch_s == 60);
  CHECK(saved.duration_ms == 45000);
  History rebooted;
  CHECK(rebooted.restore(history.snapshot()));
  CHECK(rebooted.record(0).start_epoch_s == saved.start_epoch_s);
  CHECK(rebooted.record(0).end_epoch_s == saved.end_epoch_s);
  History boundary;
  CHECK(boundary.add(session(), 72000, heater_history::MIN_VALID_EPOCH_S + 5));
  CHECK(boundary.record(0).start_epoch_s == 0 && boundary.record(0).end_epoch_s != 0);
  CHECK(!boundary.synchronize_clock(90000, EPOCH));
  CHECK(boundary.record(0).start_epoch_s == 0);
  CHECK(rebooted.restore(boundary.snapshot()));
}

int main() {
  unknown_clock_restore_is_honest();
  epochs_survive_boot_and_delayed_sync();
  ring_order_and_snapshot_overflow();
  reject_corrupt_snapshot_transactionally();
  invalid_sessions_and_uptime_wrap();
  migrate_twelve_records_and_export_rollback();
  actual_start_is_not_reconstructed_from_accounted_duration();
  std::cout << "PASS: " << checks << " assertions in 7 heater-history cases\n";
}
