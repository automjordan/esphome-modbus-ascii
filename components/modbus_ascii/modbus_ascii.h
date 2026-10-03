#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"

#include <string>
#include <vector>

namespace esphome {
namespace modbus_ascii {

class ModbusAsciiSensor : public sensor::Sensor {
 public:
  void set_register(uint16_t reg) { this->reg_ = reg; }
  void set_signed(bool s) { this->signed_ = s; }
  void set_passive(bool p) { this->passive_ = p; }
  uint16_t get_register() const { return this->reg_; }
  bool is_passive() const { return this->passive_; }

  void publish_raw(uint16_t raw) {
    float v = this->signed_ ? static_cast<float>(static_cast<int16_t>(raw))
                            : static_cast<float>(raw);
    this->publish_state(v);
  }

 protected:
  uint16_t reg_{0};
  bool signed_{false};
  bool passive_{false};
};

class ModbusAscii : public PollingComponent, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_address(uint8_t a) { this->address_ = a; }
  void set_flow_control_pin(GPIOPin *p) { this->flow_control_pin_ = p; }
  void set_command_throttle(uint32_t ms) { this->throttle_ms_ = ms; }
  void set_response_timeout(uint32_t ms) { this->timeout_ms_ = ms; }

  void register_sensor(ModbusAsciiSensor *s) { this->sensors_.push_back(s); }

  // Scrittura singolo registro (FC06), accodata con priorita.
  void write_register(uint16_t reg, uint16_t value);
  // Lettura di `qty` registri da `reg` (FC03), accodata.
  void read_registers(uint16_t reg, uint16_t qty);
  // Sonda FC03 verso un indirizzo arbitrario: il risultato viene solo
  // loggato a livello INFO, non pubblicato sui sensori.
  void probe(uint8_t addr, uint16_t reg);

  // Ultimo valore noto di un registro, per read-modify-write.
  // Ritorna false se il registro non e' mai stato letto.
  bool get_register(uint16_t reg, uint16_t &out) const;

  // --- Emulazione del pannello ----------------------------------
  // La scheda esegue solo il broadcast FC16 sui registri 101-103
  // inviato dal pannello. Con l'emulazione attiva, dopo ogni
  // broadcast del pannello ne inviamo uno nostro con il codice
  // modalita' (nibble basso del 101) e il setpoint (102) desiderati,
  // lasciando invariati il byte alto del 101 e il registro 103.
  void set_override(bool on);
  bool get_override() const { return this->override_; }
  void set_desired_mode(uint8_t code) { this->desired_mode_ = code & 0x0F; this->has_mode_ = true; }
  void set_desired_setpoint(uint16_t v) { this->desired_sp_ = v; this->has_sp_ = true; }
  // force=true salta il rate limit (comandi dell'utente).
  void send_panel_frame(bool force = false);

 protected:
  struct Command {
    uint8_t addr;
    uint8_t func;
    uint16_t reg;
    uint16_t arg;  // qty per FC03, value per FC06
    uint16_t v[3];  // valori per il broadcast FC16 (101..103)
  };

  void send_(const Command &c);
  bool decode_hex_(const std::string &in, std::vector<uint8_t> &out);
  void handle_frame_(const std::vector<uint8_t> &f);
  void handle_own_response_(const std::vector<uint8_t> &f);
  void handle_foreign_frame_(const std::vector<uint8_t> &f);
  void publish_register_(uint16_t reg, uint16_t value);

  uint8_t address_{1};
  GPIOPin *flow_control_pin_{nullptr};
  uint32_t throttle_ms_{200};
  uint32_t timeout_ms_{500};

  std::vector<Command> queue_;
  bool waiting_{false};
  Command current_{};
  uint32_t sent_at_{0};
  uint32_t last_send_{0};

  std::string buffer_;
  bool in_frame_{false};

  // correlazione del traffico di un altro master (il pannello)
  bool foreign_pending_{false};
  uint16_t foreign_start_{0};

  std::vector<ModbusAsciiSensor *> sensors_;

  // cache degli ultimi valori letti (registro -> valore)
  std::vector<std::pair<uint16_t, uint16_t>> cache_;

  // stato emulazione pannello
  bool override_{false};
  bool has_mode_{false};
  bool has_sp_{false};
  uint8_t desired_mode_{0};
  uint16_t desired_sp_{0};
  bool panel_seen_{false};
  uint16_t panel_[3]{0, 0, 0};  // ultimi 101, 102, 103 ricevuti dal pannello
  uint32_t last_emul_{0};
};

}  // namespace modbus_ascii
}  // namespace esphome
