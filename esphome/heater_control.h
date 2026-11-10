#pragma once

// E211 setpoint coordinator only: no GPIO/UART, clock, scheduler or transmission
// lives here. Serialize all calls in the ESPHome main loop. The caller owns the
// physical GPIO25 transport, response parser, one-outstanding-request discipline
// and user authorization. Call next_action() only when the shared parser is idle,
// dispatch the returned action ONCE, then call on_sent() exactly once.
//
// reading() accepts ONLY validated, correlated GPIO26 responses. raw for 060E
// is the parser's U16LE value; its low byte is the table index, not degrees.
// Local UART echo/command ACK must never be passed as a setpoint confirmation.
// A failed/unknown transmission or missing/unexpected readback ends the request;
// no relative command is automatically retried. A new explicit request always
// starts with a fresh 060E query before any INC/DEC can be proposed.

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace rinnai_control {

enum class Kind : uint8_t { NONE, QUERY_INDEX, INCREMENT, DECREMENT };

struct Action {
  Kind kind{Kind::NONE};
  uint16_t address{0x060E};
  uint32_t token{0};
};

inline constexpr uint32_t QUERY_TIMEOUT_MS = 2000;
inline constexpr uint32_t MAX_OBSERVATION_AGE_MS = 15000;
inline constexpr uint32_t STEP_SETTLE_MS = 200;

// APK Utils.TemperatureRange Id=3, selected by REUE211 (HeatModel Id=14).
// Indices 0..2 are zeros, NOT 32/33/34 degrees. The APK's hotter entries are
// deliberately omitted: this controller permits exactly 35..50 C, indices3..18.
inline constexpr std::array<uint8_t, 19> E211_TEMPERATURE{{
    0, 0, 0, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50}};

inline constexpr int temperature_for_index(uint32_t index) {
  return index >= 3 && index < E211_TEMPERATURE.size() ? E211_TEMPERATURE[index] : -1;
}

inline constexpr int index_for_temperature(int temperature) {
  return temperature >= 35 && temperature <= 50 ? temperature - 32 : -1;
}

static_assert(temperature_for_index(0) == -1 && temperature_for_index(2) == -1);
static_assert(temperature_for_index(6) == 38 && temperature_for_index(18) == 50);
static_assert(temperature_for_index(19) == -1 && index_for_temperature(34) == -1);

class Controller {
 public:
  // The caller supplies timestamps from accepted physical response frames,
  // not from the time it last called this setter. Unknown/stale error state or
  // stale/lost communications blocks control even when a cached index exists.
  void update_observation(bool comms_ok, bool error_known, uint16_t error_code,
                          uint32_t last_valid_ms, uint32_t last_error_ms) {
    observation_known_ = true;
    comms_ok_ = comms_ok;
    error_known_ = error_known;
    error_code_ = error_code;
    last_valid_ms_ = last_valid_ms;
    last_error_ms_ = last_error_ms;
  }

  bool target_request(int temperature, uint32_t now_ms) {
    if (busy()) {
      set_status_("Pedido recusado: controle ja ocupado; alvo atual mantido");
      return false;
    }
    if (index_for_temperature(temperature) < 0) {
      fail_("Pedido recusado: temperatura inteira deve estar entre 35 e 50 C");
      return false;
    }
    const char *reason = nullptr;
    if (!observations_safe_(now_ms, reason)) {
      fail_(reason);
      return false;
    }
    desired_ = temperature;
    desired_index_ = index_for_temperature(temperature);
    dispatched_steps_ = 0;
    confirmed_steps_ = 0;
    before_index_ = -1;
    expected_index_ = -1;
    pending_ = Action{};
    phase_ = Phase::NEED_BEFORE;
    phase_since_ms_ = now_ms;
    // An old uncertain step is not repeated: the next action is ALWAYS a read.
    set_status_("Pedido aceito; consultar 060E antes de qualquer ajuste");
    return true;
  }

  // Convenience spelling for callers using the earlier task terminology.
  bool request_target(int temperature, uint32_t now_ms) { return target_request(temperature, now_ms); }

  bool busy() const {
    return phase_ != Phase::IDLE && phase_ != Phase::DONE && phase_ != Phase::FAILED;
  }
  int desired() const { return desired_; }
  bool succeeded() const { return phase_ == Phase::DONE; }
  int actual_index() const { return has_index_ ? actual_index_ : -1; }
  int actual_celsius() const { return has_index_ ? temperature_for_index(actual_index_) : -1; }
  uint32_t index_age_ms(uint32_t now_ms) const {
    return has_index_ ? static_cast<uint32_t>(now_ms - last_index_ms_) : std::numeric_limits<uint32_t>::max();
  }
  uint32_t observation_age_ms(uint32_t now_ms) const {
    return observation_known_ ? static_cast<uint32_t>(now_ms - last_valid_ms_)
                              : std::numeric_limits<uint32_t>::max();
  }
  uint32_t error_age_ms(uint32_t now_ms) const {
    return error_known_ ? static_cast<uint32_t>(now_ms - last_error_ms_)
                        : std::numeric_limits<uint32_t>::max();
  }
  unsigned confirmed_steps() const { return confirmed_steps_; }
  bool uncertain() const { return uncertain_; }
  const std::string &status() const { return status_; }

  void cancel() {
    fail_(uncertain_ ? "Cancelado; passo pode ter ocorrido, consultar 060E antes de novo pedido"
                    : "Controle cancelado; nenhum novo comando sera proposto");
  }

  // This method never sends anything. Every actionable state expires; neither
  // an unacknowledged dispatch nor a response timeout can reissue an action.
  void tick(uint32_t now_ms) {
    if (!busy())
      return;
    const char *reason = nullptr;
    if (!observations_safe_(now_ms, reason)) {
      fail_(reason);
      return;
    }
    if (static_cast<uint32_t>(now_ms - phase_since_ms_) >= QUERY_TIMEOUT_MS) {
      fail_(uncertain_ ? "Timeout: ajuste sem confirmacao; interrompido, sem repetir INC/DEC"
                       : "Timeout: consulta/envio sem confirmacao em 2 s; controle interrompido");
    }
  }

  Action next_action(uint32_t now_ms) {
    tick(now_ms);
    if (!busy())
      return Action{};
    if (phase_ == Phase::NEED_BEFORE)
      return reserve_(Kind::QUERY_INDEX, Phase::DISPATCH_BEFORE, now_ms);
    if (phase_ == Phase::NEED_AFTER) {
      if (static_cast<uint32_t>(now_ms - step_sent_ms_) < STEP_SETTLE_MS)
        return Action{};
      return reserve_(Kind::QUERY_INDEX, Phase::DISPATCH_AFTER, now_ms);
    }
    if (phase_ != Phase::NEED_STEP)
      return Action{};
    if (!has_index_ || actual_index_ != before_index_ || index_age_ms(now_ms) >= QUERY_TIMEOUT_MS) {
      fail_("Setpoint mudou ou envelheceu antes do passo; controle interrompido");
      return Action{};
    }
    if (dispatched_steps_ >= 15) {
      fail_("Limite de 15 passos atingido; novo pedido explicito necessario");
      return Action{};
    }
    const int direction = desired_index_ > before_index_ ? 1 : -1;
    expected_index_ = before_index_ + direction;
    if (temperature_for_index(expected_index_) < 0) {
      fail_("Passo recusado: ultrapassaria a faixa permitida de 35 a 50 C");
      return Action{};
    }
    ++dispatched_steps_;
    // Once handed to the caller, a command may have reached the wire even if
    // the caller later fails to report local TX. Only a readback clears this.
    uncertain_ = true;
    return reserve_(direction > 0 ? Kind::INCREMENT : Kind::DECREMENT,
                    Phase::DISPATCH_STEP, now_ms);
  }

  void on_sent(const Action &action, bool tx_ok, uint32_t now_ms) {
    tick(now_ms);
    if (!busy())
      return;
    if (action.kind == Kind::NONE || action.token == 0 || action.token != pending_.token ||
        action.kind != pending_.kind || action.address != 0x060E) {
      fail_("Confirmacao de envio fora de ordem; controle interrompido");
      return;
    }
    const Phase dispatched = phase_;
    if (dispatched != Phase::DISPATCH_BEFORE && dispatched != Phase::DISPATCH_AFTER &&
        dispatched != Phase::DISPATCH_STEP) {
      fail_("Confirmacao duplicada/invalida; nenhum comando sera repetido");
      return;
    }
    pending_ = Action{};
    if (!tx_ok) {
      fail_(uncertain_ ? "Falha no TX do passo; resultado incerto, sem repeticao automatica"
                       : "Falha no TX da consulta; controle interrompido");
      return;
    }
    phase_since_ms_ = now_ms;
    if (dispatched == Phase::DISPATCH_STEP) {
      step_sent_ms_ = now_ms;
      phase_ = Phase::NEED_AFTER;
      set_status_("Passo enviado; aguardar 200 ms e consultar 060E para confirmar");
    } else {
      phase_ = dispatched == Phase::DISPATCH_BEFORE ? Phase::WAIT_BEFORE : Phase::WAIT_AFTER;
      set_status_(dispatched == Phase::DISPATCH_BEFORE ? "Aguardando indice 060E antes do passo"
                                                    : "Aguardando indice 060E apos o passo");
    }
  }

  void reading(uint16_t address, uint32_t raw, uint32_t now_ms) {
    if (address != 0x060E)
      return;
    tick(now_ms);
    const int index = raw <= 0xFFFF ? static_cast<int>(raw & 0xFFU) : -1;
    if (temperature_for_index(index) < 0) {
      has_index_ = false;
      if (busy())
        fail_("Indice 060E invalido/fora da faixa E211 de 35 a 50 C; ajuste bloqueado");
      return;
    }
    actual_index_ = index;
    has_index_ = true;
    last_index_ms_ = now_ms;
    if (!busy()) {
      // A later periodic read can establish the physical state after failure,
      // but NEVER resumes an abandoned target or emits any action.
      uncertain_ = false;
      return;
    }
    if (phase_ == Phase::WAIT_BEFORE) {
      uncertain_ = false;
      before_index_ = index;
      if (index == desired_index_) {
        complete_();
      } else {
        phase_ = Phase::NEED_STEP;
        phase_since_ms_ = now_ms;
        set_status_("Indice atual confirmado; proximo passo relativo permitido");
      }
    } else if (phase_ == Phase::WAIT_AFTER) {
      uncertain_ = false;
      if (index != expected_index_) {
        fail_(index == before_index_ ? "Passo nao confirmado; sem repetir INC/DEC automaticamente"
                                    : "Setpoint inesperado apos passo; controle interrompido");
        return;
      }
      ++confirmed_steps_;
      if (index == desired_index_) {
        complete_();
      } else {
        // Separate fresh query before EVERY step, even after the preceding
        // step's successful readback. No blind multi-step write sequence.
        phase_ = Phase::NEED_BEFORE;
        phase_since_ms_ = now_ms;
        set_status_("Passo confirmado; consultar novamente 060E antes do proximo");
      }
    } else if (phase_ == Phase::NEED_STEP && index != before_index_) {
      fail_("Setpoint alterado externamente antes do passo; controle interrompido");
    }
  }

 private:
  enum class Phase : uint8_t {
    IDLE, NEED_BEFORE, DISPATCH_BEFORE, WAIT_BEFORE, NEED_STEP, DISPATCH_STEP,
    NEED_AFTER, DISPATCH_AFTER, WAIT_AFTER, DONE, FAILED
  };

  bool observations_safe_(uint32_t now_ms, const char *&reason) const {
    if (!observation_known_ || !comms_ok_) {
      reason = "Ajuste bloqueado: comunicacao nao confirmada";
    } else if (static_cast<uint32_t>(now_ms - last_valid_ms_) > MAX_OBSERVATION_AGE_MS) {
      reason = "Ajuste bloqueado: telemetria com mais de 15 s";
    } else if (!error_known_) {
      reason = "Ajuste bloqueado: estado de erro desconhecido";
    } else if (static_cast<uint32_t>(now_ms - last_error_ms_) > MAX_OBSERVATION_AGE_MS) {
      reason = "Ajuste bloqueado: consulta de erro com mais de 15 s";
    } else if (error_code_ != 0) {
      reason = "Ajuste bloqueado: aquecedor informa erro ativo";
    } else {
      return true;
    }
    return false;
  }

  Action reserve_(Kind kind, Phase dispatched, uint32_t now_ms) {
    ++token_counter_;
    if (token_counter_ == 0)
      ++token_counter_;
    pending_ = Action{kind, 0x060E, token_counter_};
    phase_ = dispatched;
    phase_since_ms_ = now_ms;
    return pending_;
  }
  void set_status_(const char *text) { status_ = text; }
  void fail_(const char *text) {
    phase_ = Phase::FAILED;
    pending_ = Action{};
    set_status_(text);
  }
  void complete_() {
    phase_ = Phase::DONE;
    pending_ = Action{};
    uncertain_ = false;
    char text[100];
    std::snprintf(text, sizeof(text), "Setpoint confirmado: %d C (indice %d), %u passo(s)",
                  actual_celsius(), actual_index_, confirmed_steps_);
    status_ = text;
  }

  Phase phase_{Phase::IDLE};
  Action pending_{};
  uint32_t token_counter_{0};
  uint32_t phase_since_ms_{0};
  uint32_t step_sent_ms_{0};
  uint32_t last_index_ms_{0};
  uint32_t last_valid_ms_{0};
  uint32_t last_error_ms_{0};
  bool observation_known_{false};
  bool comms_ok_{false};
  bool error_known_{false};
  bool has_index_{false};
  bool uncertain_{false};
  uint16_t error_code_{0};
  int actual_index_{-1};
  int before_index_{-1};
  int expected_index_{-1};
  int desired_{-1};
  int desired_index_{-1};
  unsigned dispatched_steps_{0};
  unsigned confirmed_steps_{0};
  std::string status_{"Aguardando pedido explicito e telemetria valida"};
};

}  // namespace rinnai_control
