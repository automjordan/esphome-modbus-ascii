#pragma once

#include "esphome/core/component.h"
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
  uint16_t get_register() const { return this->reg_; }

  void publish_raw(uint16_t raw) {
    float v = this->signed_ ? static_cast<float>(static_cast<int16_t>(raw))
                            : static_cast<float>(raw);
    this->publish_state(v);
  }

 protected:
  uint16_t reg_{0};
  bool signed_{false};
};

class ModbusAsciiSniffer : public Component, public uart::UARTDevice {
 public:
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void register_sensor(ModbusAsciiSensor *s) { this->sensors_.push_back(s); }

 protected:
  bool decode_hex_(const std::string &in, std::vector<uint8_t> &out);
  void process_frame_(const std::vector<uint8_t> &f);
  void publish_register_(uint16_t reg, uint16_t value);

  std::string buffer_;
  bool in_frame_{false};

  bool pending_valid_{false};
  uint16_t pending_start_{0};
  uint16_t pending_qty_{0};

  std::vector<ModbusAsciiSensor *> sensors_;
};

}  // namespace modbus_ascii
}  // namespace esphome
