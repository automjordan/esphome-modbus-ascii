#include "modbus_ascii.h"
#include "esphome/core/log.h"

namespace esphome {
namespace modbus_ascii {

static const char *const TAG = "modbus_ascii";

static int hex_val(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

void ModbusAsciiSniffer::dump_config() {
  ESP_LOGCONFIG(TAG, "Modbus ASCII sniffer:");
  ESP_LOGCONFIG(TAG, "  Sensori registrati: %u", (unsigned) this->sensors_.size());
}

void ModbusAsciiSniffer::loop() {
  while (this->available()) {
    uint8_t c;
    this->read_byte(&c);

    if (c == ':') {
      this->in_frame_ = true;
      this->buffer_.clear();
      continue;
    }
    if (!this->in_frame_)
      continue;

    if (c == '\r')
      continue;

    if (c == '\n') {
      this->in_frame_ = false;
      std::vector<uint8_t> frame;
      if (this->decode_hex_(this->buffer_, frame)) {
        this->process_frame_(frame);
      } else {
        ESP_LOGD(TAG, "Frame scartato: :%s", this->buffer_.c_str());
      }
      this->buffer_.clear();
      continue;
    }

    if (this->buffer_.size() > 512) {
      this->in_frame_ = false;
      this->buffer_.clear();
      continue;
    }
    this->buffer_.push_back((char) c);
  }
}

bool ModbusAsciiSniffer::decode_hex_(const std::string &in, std::vector<uint8_t> &out) {
  if (in.size() < 6 || (in.size() % 2) != 0)
    return false;

  out.clear();
  for (size_t i = 0; i + 1 < in.size(); i += 2) {
    int hi = hex_val(in[i]);
    int lo = hex_val(in[i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    out.push_back((uint8_t) ((hi << 4) | lo));
  }

  uint8_t lrc = 0;
  for (size_t i = 0; i + 1 < out.size(); i++)
    lrc = (uint8_t) (lrc + out[i]);
  lrc = (uint8_t) (-((int8_t) lrc));

  if (lrc != out.back()) {
    ESP_LOGD(TAG, "LRC errato (atteso %02X, ricevuto %02X)", lrc, out.back());
    return false;
  }

  out.pop_back();
  return true;
}

void ModbusAsciiSniffer::process_frame_(const std::vector<uint8_t> &f) {
  if (f.size() < 2)
    return;

  const uint8_t addr = f[0];
  const uint8_t func = f[1];
  const size_t n = f.size();

  std::string dump;
  for (size_t i = 0; i < n; i++) {
    char b[4];
    sprintf(b, "%02X ", f[i]);
    dump += b;
  }
  ESP_LOGD(TAG, "slave=%u func=%02X | %s", addr, func, dump.c_str());

  if (func == 0x03) {
    const bool looks_like_response =
        (n > 3) && (f[2] == (uint8_t) (n - 3)) && (f[2] % 2 == 0) && (f[2] > 0);

    if (looks_like_response) {
      if (!this->pending_valid_) {
        ESP_LOGD(TAG, "Risposta senza richiesta correlata, ignorata");
        return;
      }
      const uint16_t count = f[2] / 2;
      for (uint16_t i = 0; i < count; i++) {
        const uint16_t value = (uint16_t) ((f[3 + i * 2] << 8) | f[4 + i * 2]);
        this->publish_register_((uint16_t) (this->pending_start_ + i), value);
      }
      this->pending_valid_ = false;
      return;
    }

    if (n == 6) {
      this->pending_start_ = (uint16_t) ((f[2] << 8) | f[3]);
      this->pending_qty_ = (uint16_t) ((f[4] << 8) | f[5]);
      this->pending_valid_ = true;
      ESP_LOGD(TAG, "Richiesta lettura: start=%u qty=%u", this->pending_start_,
               this->pending_qty_);
    }
    return;
  }

  if (func == 0x06 && n == 6) {
    const uint16_t reg = (uint16_t) ((f[2] << 8) | f[3]);
    const uint16_t value = (uint16_t) ((f[4] << 8) | f[5]);
    ESP_LOGD(TAG, "Scrittura registro %u = %u", reg, value);
    this->publish_register_(reg, value);
    return;
  }

  if (func & 0x80) {
    ESP_LOGW(TAG, "Eccezione Modbus da slave %u: codice %02X", addr,
             n > 2 ? f[2] : 0);
    this->pending_valid_ = false;
  }
}

void ModbusAsciiSniffer::publish_register_(uint16_t reg, uint16_t value) {
  for (auto *s : this->sensors_) {
    if (s->get_register() == reg)
      s->publish_raw(value);
  }
}

}  // namespace modbus_ascii
}  // namespace esphome
