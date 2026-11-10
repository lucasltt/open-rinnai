// Pure command metadata tests. No serial traffic, networking or persistence.
#include "../esphome/api_command.h"
#include <cstdlib>
#include <iostream>
#include <string>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; std::exit(EXIT_FAILURE); } } while (false)
using rinnai_api::CommandTracker;
using rinnai_api::Result;

int main() {
  CommandTracker command;
  CHECK(command.result() == Result::IDLE && !command.active());
  CHECK(command.id() == 0 && command.boot_id() == 0 && command.target() == -1);
  CHECK(std::string(command.result_name()) == "idle");
  CHECK(!command.cancel(1));
  command.started(true, 2);
  command.observe(false, true, 38, false, 3);
  CHECK(command.result() == Result::IDLE);

  CHECK(command.submitted(39, 18, 100) == 1);
  CHECK(command.active() && command.result() == Result::QUEUED);
  CHECK(command.boot_id() == 18 && command.target() == 39);
  CHECK(command.accepted_ms() == 100 && command.updated_ms() == 100);
  CHECK(std::string(command.result_name()) == "queued");
  command.observe(false, true, 39, false, 110);
  CHECK(command.result() == Result::QUEUED); // A cached matching value cannot complete a queued operation.
  command.started(true, 120);
  CHECK(command.result() == Result::RUNNING && command.active());
  CHECK(command.accepted_ms() == 100 && command.updated_ms() == 120);
  CHECK(std::string(command.result_name()) == "running");
  command.started(false, 125);
  CHECK(command.result() == Result::RUNNING && command.updated_ms() == 120);
  command.observe(true, true, 39, false, 130);
  CHECK(command.result() == Result::RUNNING); // Busy wins over a temporarily matching setpoint.
  command.observe(false, true, 39, false, 140);
  CHECK(command.result() == Result::CONFIRMED && !command.active());
  CHECK(command.updated_ms() == 140 && command.accepted_ms() == 100);
  CHECK(std::string(command.result_name()) == "confirmed");
  command.observe(false, false, 38, true, 150);
  command.started(false, 151);
  CHECK(!command.cancel(152));
  CHECK(command.result() == Result::CONFIRMED && command.updated_ms() == 140);

  CHECK(command.submitted(38, 18, 200) == 2);
  command.started(false, 210);
  CHECK(command.result() == Result::FAILED && !command.active());
  CHECK(command.updated_ms() == 210);
  CHECK(std::string(command.result_name()) == "failed");
  command.observe(false, true, 38, false, 220);
  CHECK(command.result() == Result::FAILED); // A later observation cannot resurrect a failed command.
  CHECK(command.submitted(38, 18, 300) == 3);
  CHECK(command.cancel(301));
  CHECK(command.result() == Result::CANCELLED && !command.active());
  CHECK(std::string(command.result_name()) == "cancelled");
  CHECK(command.updated_ms() == 301 && command.accepted_ms() == 300);
  command.started(true, 302);
  command.observe(false, true, 38, false, 303);
  CHECK(command.result() == Result::CANCELLED && !command.cancel(304));

  CHECK(command.submitted(40, 18, 400) == 4);
  command.started(true, 401);
  CHECK(command.cancel(402));
  command.observe(false, true, 40, false, 403);
  CHECK(command.result() == Result::CANCELLED); // A late readback does not change cancellation metadata.

  for (const auto scenario : {0, 1, 2}) {
    command.submitted(40, 18, 500 + scenario * 10);
    command.started(true, 501 + scenario * 10);
    command.observe(false, scenario != 0, scenario == 1 ? 39 : 40, scenario == 2, 502 + scenario * 10);
    CHECK(command.result() == Result::FAILED && !command.active());
  }
  CHECK(command.submitted(35, 19, 0xFFFFFFF0U) == 1);
  CHECK(command.boot_id() == 19 && command.accepted_ms() == 0xFFFFFFF0U);
  command.started(true, 0xFFFFFFF8U);
  command.observe(false, true, 35, false, 5);
  CHECK(command.result() == Result::CONFIRMED && command.updated_ms() == 5);
  CHECK(command.submitted(50, 19, 10) == 2);
  CHECK(command.submitted(50, 20, 20) == 1);
  std::cout << "API command tracker: " << checks << " checks passed\n";
}
