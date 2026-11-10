// Host-only controller tests: no transport, ESP-IDF, GPIO or network.
#include "../esphome/heater_control.h"

#include <cstdlib>
#include <iostream>

using rinnai_control::Action;
using rinnai_control::Controller;
using rinnai_control::Kind;
static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; \
  std::exit(EXIT_FAILURE); } } while (false)

struct Fixture {
  Controller controller;
  uint32_t now{1000};
  void fresh() { controller.update_observation(true, true, 0, now, now); }
  Fixture() { fresh(); }
  void before(uint32_t raw) {
    const Action query = controller.next_action(now);
    CHECK(query.kind == Kind::QUERY_INDEX);
    CHECK(query.address == 0x060E);
    CHECK(query.token != 0);
    CHECK(controller.next_action(now).kind == Kind::NONE);
    controller.on_sent(query, true, ++now);
    controller.reading(0x060E, raw, ++now);
  }
  Action send_step(Kind kind) {
    const Action step = controller.next_action(++now);
    CHECK(step.kind == kind);
    CHECK(controller.uncertain());
    CHECK(controller.next_action(now).kind == Kind::NONE);
    controller.on_sent(step, true, ++now);
    return step;
  }
  void after(uint32_t raw) {
    now += 199;
    CHECK(controller.next_action(now).kind == Kind::NONE);
    ++now;
    const Action query = controller.next_action(now);
    CHECK(query.kind == Kind::QUERY_INDEX);
    controller.on_sent(query, true, ++now);
    controller.reading(0x060E, raw, ++now);
  }
};

static void round_trip_and_noop() {
  Fixture f;
  CHECK(f.controller.target_request(39, f.now));
  f.before(0x0106);  // High byte is an independent flag, low byte is index 6.
  CHECK(f.controller.actual_celsius() == 38);
  f.send_step(Kind::INCREMENT);
  f.after(0x0107);
  CHECK(!f.controller.busy());
  CHECK(!f.controller.uncertain());
  CHECK(f.controller.actual_celsius() == 39);
  CHECK(f.controller.confirmed_steps() == 1);
  CHECK(f.controller.next_action(f.now).kind == Kind::NONE);
  CHECK(f.controller.target_request(38, f.now));
  f.before(0x0007);
  f.send_step(Kind::DECREMENT);
  f.after(0x0006);
  CHECK(!f.controller.busy());
  CHECK(f.controller.actual_celsius() == 38);
  CHECK(f.controller.confirmed_steps() == 1);
  CHECK(f.controller.target_request(38, f.now));
  f.before(0x0106);
  CHECK(!f.controller.busy());
  CHECK(f.controller.confirmed_steps() == 0);
  CHECK(f.controller.next_action(f.now).kind == Kind::NONE);
}

static void wrong_readback_never_retries() {
  for (const uint32_t wrong : {6U, 8U}) {
    Fixture f;
    CHECK(f.controller.target_request(39, f.now));
    f.before(6);
    f.send_step(Kind::INCREMENT);
    f.after(wrong);
    CHECK(!f.controller.busy());
    CHECK(f.controller.confirmed_steps() == 0);
    CHECK(f.controller.next_action(f.now).kind == Kind::NONE);
    f.controller.reading(0x060E, 6, ++f.now);
    CHECK(f.controller.next_action(f.now).kind == Kind::NONE);
  }
  Fixture external;
  CHECK(external.controller.target_request(39, external.now));
  external.before(6);
  external.controller.reading(0x060E, 8, ++external.now);
  CHECK(!external.controller.busy());
  CHECK(external.controller.next_action(external.now).kind == Kind::NONE);
}

static void stale_unknown_error_and_busy() {
  Controller c;
  CHECK(!c.target_request(39, 1000));
  c.update_observation(true, false, 0, 1000, 1000);
  CHECK(!c.target_request(39, 1000));
  c.update_observation(true, true, 11, 1000, 1000);
  CHECK(!c.target_request(39, 1000));
  c.update_observation(false, true, 0, 1000, 1000);
  CHECK(!c.target_request(39, 1000));
  c.update_observation(true, true, 0, 20000, 0);
  CHECK(!c.target_request(39, 20000));
  c.update_observation(true, true, 0, 0, 20000);
  CHECK(!c.target_request(39, 20000));
  c.update_observation(true, true, 0, 5000, 5000);
  CHECK(c.target_request(39, 20000));  // Exactly 15 seconds is permitted.
  CHECK(!c.target_request(40, 20000));
  CHECK(c.desired() == 39);
  CHECK(c.busy());
  c.update_observation(true, true, 12, 20000, 20000);
  c.tick(20000);
  CHECK(!c.busy());
  CHECK(c.next_action(20000).kind == Kind::NONE);
}

static void timeouts_and_uncertain_send() {
  Fixture query;
  CHECK(query.controller.target_request(39, query.now));
  Action action = query.controller.next_action(query.now);
  query.controller.on_sent(action, true, ++query.now);
  query.now += 2000;
  query.controller.tick(query.now);
  CHECK(!query.controller.busy());
  CHECK(!query.controller.uncertain());
  CHECK(query.controller.next_action(query.now).kind == Kind::NONE);

  Fixture step;
  CHECK(step.controller.target_request(39, step.now));
  step.before(6);
  step.send_step(Kind::INCREMENT);
  step.now += 200;
  action = step.controller.next_action(step.now);
  CHECK(action.kind == Kind::QUERY_INDEX);
  step.controller.on_sent(action, true, ++step.now);
  step.now += 2000;
  step.controller.tick(step.now);
  CHECK(!step.controller.busy());
  CHECK(step.controller.uncertain());
  CHECK(step.controller.next_action(step.now).kind == Kind::NONE);
  CHECK(step.controller.target_request(39, step.now));
  CHECK(step.controller.next_action(step.now).kind == Kind::QUERY_INDEX);

  Fixture failed_tx;
  CHECK(failed_tx.controller.target_request(39, failed_tx.now));
  failed_tx.before(6);
  action = failed_tx.controller.next_action(++failed_tx.now);
  CHECK(action.kind == Kind::INCREMENT);
  failed_tx.controller.on_sent(action, false, ++failed_tx.now);
  CHECK(failed_tx.controller.uncertain());
  CHECK(!failed_tx.controller.busy());
  CHECK(failed_tx.controller.next_action(failed_tx.now).kind == Kind::NONE);

  Fixture never_acknowledged;
  CHECK(never_acknowledged.controller.target_request(39, never_acknowledged.now));
  never_acknowledged.before(6);
  CHECK(never_acknowledged.controller.next_action(++never_acknowledged.now).kind == Kind::INCREMENT);
  never_acknowledged.now += 2000;
  never_acknowledged.controller.tick(never_acknowledged.now);
  CHECK(never_acknowledged.controller.uncertain());
  CHECK(never_acknowledged.controller.next_action(never_acknowledged.now).kind == Kind::NONE);
}

static void ordering_tokens() {
  Fixture wrong;
  CHECK(wrong.controller.target_request(39, wrong.now));
  Action action = wrong.controller.next_action(wrong.now);
  ++action.token;
  wrong.controller.on_sent(action, true, ++wrong.now);
  CHECK(!wrong.controller.busy());
  CHECK(wrong.controller.next_action(wrong.now).kind == Kind::NONE);

  Fixture duplicate;
  CHECK(duplicate.controller.target_request(39, duplicate.now));
  action = duplicate.controller.next_action(duplicate.now);
  duplicate.controller.on_sent(action, true, ++duplicate.now);
  duplicate.controller.on_sent(action, true, ++duplicate.now);
  CHECK(!duplicate.controller.busy());
  CHECK(duplicate.controller.next_action(duplicate.now).kind == Kind::NONE);

  Fixture address;
  CHECK(address.controller.target_request(39, address.now));
  action = address.controller.next_action(address.now);
  action.address = 0x0604;
  address.controller.on_sent(action, true, ++address.now);
  CHECK(!address.controller.busy());
}

static void bounds_and_indices() {
  using rinnai_control::index_for_temperature;
  using rinnai_control::temperature_for_index;
  for (int temperature = 35; temperature <= 50; ++temperature) {
    CHECK(temperature_for_index(static_cast<uint32_t>(index_for_temperature(temperature))) == temperature);
    Fixture f;
    CHECK(f.controller.target_request(temperature, f.now));
    f.before(static_cast<uint32_t>(index_for_temperature(temperature)));
    CHECK(!f.controller.busy());
  }
  for (const int invalid : {-100, 0, 34, 51, 1000}) {
    Fixture f;
    CHECK(!f.controller.target_request(invalid, f.now));
    CHECK(f.controller.next_action(f.now).kind == Kind::NONE);
  }
  for (const uint32_t index : {0U, 1U, 2U, 19U, 255U, 65536U}) {
    Fixture f;
    CHECK(f.controller.target_request(39, f.now));
    f.before(index);
    CHECK(!f.controller.busy());
    CHECK(f.controller.actual_celsius() == -1);
    CHECK(f.controller.next_action(f.now).kind == Kind::NONE);
  }
  Fixture flag;
  CHECK(flag.controller.target_request(38, flag.now));
  flag.before(0xFF06);
  CHECK(flag.controller.actual_celsius() == 38);
  CHECK(!flag.controller.busy());
}

static void maximum_span_and_rollover() {
  Fixture f;
  CHECK(f.controller.target_request(50, f.now));
  for (uint32_t index = 3; index < 18; ++index) {
    f.before(index);
    f.send_step(Kind::INCREMENT);
    f.after(index + 1);
  }
  CHECK(!f.controller.busy());
  CHECK(f.controller.actual_celsius() == 50);
  CHECK(f.controller.confirmed_steps() == 15);
  CHECK(f.controller.next_action(f.now).kind == Kind::NONE);

  Fixture wrap;
  wrap.now = UINT32_MAX - 100;
  wrap.fresh();
  CHECK(wrap.controller.target_request(39, wrap.now));
  wrap.before(6);
  wrap.send_step(Kind::INCREMENT);
  wrap.after(7);
  CHECK(wrap.controller.actual_celsius() == 39);
  CHECK(!wrap.controller.busy());
}

int main() {
  round_trip_and_noop();
  wrong_readback_never_retries();
  stale_unknown_error_and_busy();
  timeouts_and_uncertain_send();
  ordering_tokens();
  bounds_and_indices();
  maximum_span_and_rollover();
  std::cout << "PASS: " << checks << " checks across 7 heater control scenarios\n";
}
