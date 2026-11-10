#pragma once
#include <cstdint>

namespace rinnai_api {
enum class Result { IDLE, QUEUED, RUNNING, CONFIRMED, FAILED, CANCELLED };
// Metadata only. The backend mutex protects this; it never sends heater data.
class CommandTracker {
 public:
  uint32_t submitted(int target, uint32_t boot, uint32_t now) {
    if (boot_ != boot) id_ = 0;
    if (++id_ == 0) ++id_;
    boot_ = boot; target_ = target; accepted_ = updated_ = now; result_ = Result::QUEUED;
    return id_;
  }
  void started(bool accepted, uint32_t now) {
    if (result_ != Result::QUEUED) return;
    result_ = accepted ? Result::RUNNING : Result::FAILED; updated_ = now;
  }
  void observe(bool busy, bool completed, int actual, bool uncertain, uint32_t now) {
    if (result_ != Result::RUNNING || busy) return;
    result_ = completed && actual == target_ && !uncertain ? Result::CONFIRMED : Result::FAILED;
    updated_ = now;
  }
  bool cancel(uint32_t now) {
    if (!active()) return false;
    result_ = Result::CANCELLED; updated_ = now; return true;
  }
  bool active() const { return result_ == Result::QUEUED || result_ == Result::RUNNING; }
  uint32_t id() const { return id_; }
  uint32_t boot_id() const { return boot_; }
  int target() const { return target_; }
  uint32_t accepted_ms() const { return accepted_; }
  uint32_t updated_ms() const { return updated_; }
  Result result() const { return result_; }
  const char *result_name() const {
    switch (result_) {
      case Result::QUEUED: return "queued";
      case Result::RUNNING: return "running";
      case Result::CONFIRMED: return "confirmed";
      case Result::FAILED: return "failed";
      case Result::CANCELLED: return "cancelled";
      default: return "idle";
    }
  }
 private:
  uint32_t id_{0}, boot_{0}, accepted_{0}, updated_{0};
  int target_{-1};
  Result result_{Result::IDLE};
};
}  // namespace rinnai_api
