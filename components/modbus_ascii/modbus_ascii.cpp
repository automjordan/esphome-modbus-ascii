#include "modbus_ascii.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstdio>

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

static std::string hex_dump(const std::vector<uint8_t> &f) {
  std::string s;
  char b[4];
  for (uint8_t x : f) {
    snprintf(b, sizeof(b), "%02X ", x);
    s += b;
  }
  return s;
}

// ------------------------------------------------------------------
// Ciclo di vita
// ------------------------------------------------------------------

void ModbusAscii::setup() {
  if (this->flow_control_pin_ != nullptr) {
    this->flow_control_pin_->setup();
    this->flow_control_pin_->digital_write(false);
  }
}

void ModbusAscii::dump_config() {
  ESP_LOGCONFIG(TAG, "Modbus ASCII master:");
  ESP_LOGCONFIG(TAG, "  Indirizzo slave: %u", this->address_);
  ESP_LOGCONFIG(TAG, "  Throttle: %u ms, timeout: %u ms", (unsigned) this->throttle_ms_,
                (unsigned) this->timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Sensori: %u", (unsigned) this->sensors_.size());
  LOG_UPDATE_INTERVAL(this);
}

void ModbusAscii::update() {
  // Accoda una lettura per ogni sensore attivo. Registri contigui
  // vengono raggruppati in un'unica richiesta.
  std::vector<uint16_t> regs;
  for (auto *s : this->sensors_) {
    if (!s->is_passive())
      regs.push_back(s->get_register());
  }
  if (regs.empty())
    return;
  std::sort(regs.begin(), regs.end());
  regs.erase(std::unique(regs.begin(), regs.end()), regs.end());

  uint16_t start = regs[0];
  uint16_t qty = 1;
  for (size_t i = 1; i < regs.size(); i++) {
    if (regs[i] == start + qty && qty < 16) {
      qty++;
    } else {
      this->read_registers(start, qty);
      start = regs[i];
      qty = 1;
    }
  }
  this->read_registers(start, qty);
}

void ModbusAscii::loop() {
  // --- ricezione ---------------------------------------------------
  while (this->available()) {
    uint8_t c;
    this->read_byte(&c);

    if (c == ':') {
      this->in_frame_ = true;
      this->buffer_.clear();
      continue;
    }
    if (!this->in_frame_ || c == '\r')
      continue;

    if (c == '\n') {
      this->in_frame_ = false;
      std::vector<uint8_t> frame;
      if (this->decode_hex_(this->buffer_, frame)) {
        this->handle_frame_(frame);
      } else {
        ESP_LOGV(TAG, "Frame scartata: :%s", this->buffer_.c_str());
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

  // --- gestione coda ---------------------------------------------
  const uint32_t now = millis();

  if (this->waiting_ && (now - this->sent_at_) > this->timeout_ms_) {
    ESP_LOGW(TAG, "Timeout: func=%02X reg=%u", this->current_.func, this->current_.reg);
    this->waiting_ = false;
  }

  if (!this->waiting_ && !this->queue_.empty() &&
      (now - this->last_send_) >= this->throttle_ms_) {
    this->current_ = this->queue_.front();
    this->queue_.erase(this->queue_.begin());
    this->send_(this->current_);
    this->waiting_ = true;
    this->sent_at_ = now;
    this->last_send_ = now;
  }
}

// ------------------------------------------------------------------
// API pubblica
// ------------------------------------------------------------------

void ModbusAscii::write_register(uint16_t reg, uint16_t value) {
  // Le scritture passano davanti alle letture in coda.
  this->queue_.insert(this->queue_.begin(), Command{0x06, reg, value});
}

void ModbusAscii::read_registers(uint16_t reg, uint16_t qty) {
  this->queue_.push_back(Command{0x03, reg, qty});
}

bool ModbusAscii::get_register(uint16_t reg, uint16_t &out) const {
  for (const auto &p : this->cache_) {
    if (p.first == reg) {
      out = p.second;
      return true;
    }
  }
  return false;
}

// ------------------------------------------------------------------
// Trasmissione
// ------------------------------------------------------------------

void ModbusAscii::send_(const Command &c) {
  const uint8_t payload[6] = {
      this->address_, c.func, (uint8_t) (c.reg >> 8), (uint8_t) (c.reg & 0xFF),
      (uint8_t) (c.arg >> 8), (uint8_t) (c.arg & 0xFF)};

  uint8_t lrc = 0;
  for (uint8_t b : payload)
    lrc = (uint8_t) (lrc + b);
  lrc = (uint8_t) (-((int8_t) lrc));

  char frame[20];
  snprintf(frame, sizeof(frame), ":%02X%02X%02X%02X%02X%02X%02X\r\n", payload[0], payload[1],
           payload[2], payload[3], payload[4], payload[5], lrc);

  ESP_LOGV(TAG, "TX %s", frame);

  if (this->flow_control_pin_ != nullptr)
    this->flow_control_pin_->digital_write(true);

  this->write_str(frame);
  this->flush();  // attende lo svuotamento del FIFO hardware

  if (this->flow_control_pin_ != nullptr)
    this->flow_control_pin_->digital_write(false);
}

// ------------------------------------------------------------------
// Ricezione e decodifica
// ------------------------------------------------------------------

bool ModbusAscii::decode_hex_(const std::string &in, std::vector<uint8_t> &out) {
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
    ESP_LOGD(TAG, "LRC errato su :%s", in.c_str());
    return false;
  }
  out.pop_back();
  return true;
}

void ModbusAscii::handle_frame_(const std::vector<uint8_t> &f) {
  if (f.size() < 2)
    return;

  ESP_LOGV(TAG, "RX %s", hex_dump(f).c_str());

  const uint8_t addr = f[0];
  const uint8_t func = f[1];
  const size_t n = f.size();

  // Risposta a una nostra richiesta: stesso indirizzo, stessa funzione
  // (o eccezione), e struttura da risposta e non da richiesta.
  if (this->waiting_ && addr == this->address_) {
    const bool is_exception = (func == (this->current_.func | 0x80));
    const bool is_read_resp =
        (func == 0x03 && this->current_.func == 0x03 && n > 3 && f[2] == (uint8_t) (n - 3));
    const bool is_write_echo = (func == 0x06 && this->current_.func == 0x06 && n == 6);
    if (is_exception || is_read_resp || is_write_echo) {
      this->handle_own_response_(f);
      return;
    }
  }

  this->handle_foreign_frame_(f);
}

void ModbusAscii::handle_own_response_(const std::vector<uint8_t> &f) {
  const uint8_t func = f[1];
  const size_t n = f.size();

  this->waiting_ = false;

  if (func & 0x80) {
    ESP_LOGW(TAG, "Eccezione %02X su func=%02X reg=%u", n > 2 ? f[2] : 0, this->current_.func,
             this->current_.reg);
    return;
  }

  if (func == 0x03) {
    const uint16_t count = f[2] / 2;
    for (uint16_t i = 0; i < count; i++) {
      const uint16_t value = (uint16_t) ((f[3 + i * 2] << 8) | f[4 + i * 2]);
      this->publish_register_((uint16_t) (this->current_.reg + i), value);
    }
    return;
  }

  if (func == 0x06) {
    const uint16_t reg = (uint16_t) ((f[2] << 8) | f[3]);
    const uint16_t value = (uint16_t) ((f[4] << 8) | f[5]);
    ESP_LOGD(TAG, "Scrittura confermata: reg %u = %u", reg, value);
    this->publish_register_(reg, value);
  }
}

void ModbusAscii::handle_foreign_frame_(const std::vector<uint8_t> &f) {
  const uint8_t addr = f[0];
  const uint8_t func = f[1];
  const size_t n = f.size();

  // Il pannello scrive in broadcast (FC16): catturiamo i valori.
  if (func == 0x10 && n >= 7 && f[6] == (uint8_t) (n - 7)) {
    const uint16_t start = (uint16_t) ((f[2] << 8) | f[3]);
    const uint16_t qty = (uint16_t) ((f[4] << 8) | f[5]);
    for (uint16_t i = 0; i < qty && (size_t) (8 + i * 2) < n; i++) {
      const uint16_t value = (uint16_t) ((f[7 + i * 2] << 8) | f[8 + i * 2]);
      this->publish_register_((uint16_t) (start + i), value);
    }
    return;
  }

  // Scrittura singola altrui (richiesta o echo): stesso formato.
  if (func == 0x06 && n == 6) {
    const uint16_t reg = (uint16_t) ((f[2] << 8) | f[3]);
    const uint16_t value = (uint16_t) ((f[4] << 8) | f[5]);
    this->publish_register_(reg, value);
    return;
  }

  // Lettura altrui verso la nostra scheda: memorizziamo la richiesta
  // e usiamo la risposta successiva.
  if (func == 0x03 && addr == this->address_) {
    if (n == 6) {
      this->foreign_start_ = (uint16_t) ((f[2] << 8) | f[3]);
      this->foreign_pending_ = true;
      return;
    }
    if (this->foreign_pending_ && n > 3 && f[2] == (uint8_t) (n - 3)) {
      const uint16_t count = f[2] / 2;
      for (uint16_t i = 0; i < count; i++) {
        const uint16_t value = (uint16_t) ((f[3 + i * 2] << 8) | f[4 + i * 2]);
        this->publish_register_((uint16_t) (this->foreign_start_ + i), value);
      }
      this->foreign_pending_ = false;
      return;
    }
  }

  // Tutto il resto (es. handshake 77/FF del pannello) viene ignorato.
  ESP_LOGV(TAG, "Frame altrui ignorata: addr=%u func=%02X", addr, func);
}

void ModbusAscii::publish_register_(uint16_t reg, uint16_t value) {
  bool found = false;
  for (auto &p : this->cache_) {
    if (p.first == reg) {
      p.second = value;
      found = true;
      break;
    }
  }
  if (!found)
    this->cache_.emplace_back(reg, value);

  for (auto *s : this->sensors_) {
    if (s->get_register() == reg)
      s->publish_raw(value);
  }
}

}  // namespace modbus_ascii
}  // namespace esphome
