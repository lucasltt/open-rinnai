#pragma once
#include "heater_control.h"
#include "rinnai_telemetry.h"
#include "heater_stats.h"
namespace rinnai_smart {
inline rinnai_control::Controller controller{};
inline void observe_reading(const rinnai_telemetry::Reading &reading) {
  const uint32_t now = esphome::millis();
  controller.reading(reading.address, reading.raw, now);
  using heater_stats::Signal;
  switch (reading.address) {
    case 0x0604: heater_stats::observe(Signal::OUTLET_C, reading.raw / 100.0, now); break;
    case 0x0606: heater_stats::observe(Signal::INLET_C, reading.raw / 100.0, now); break;
    case 0x0608: heater_stats::observe(Signal::FLOW_L_MIN, reading.raw / 100.0, now); break;
    case 0x0648: heater_stats::observe(Signal::POWER_KCAL_MIN, reading.raw / 10.0, now); break;
    case 0x060F: heater_stats::observe(Signal::FLAME, (reading.raw & 0x0F) == 1 ? 1 : 0, now); break;
  }
}
}
