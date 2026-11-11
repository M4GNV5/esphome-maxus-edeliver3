// Minimal UDS (ReadDataByIdentifier 0x22) poller with ISO-TP (ISO 15765-2)
// reassembly for ESPHome's canbus component. Used by maxus-edeliver3-uds.yaml
// for the Maxus eDeliver 3 BMS, but nothing in here is vehicle specific.
//
//   maxus::uds.setup(id(can0), tx_id);
//   maxus::uds.add(0xD117, 30000);                   // DID, poll interval in ms
//   maxus::uds.on_response([](uint16_t did, const std::vector<uint8_t> &data) { ... });
//   on_frame <rx_id>:  maxus::uds.handle_frame(x);
//   every 10-50 ms:    maxus::uds.loop(polling_allowed);
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/canbus/canbus.h"

namespace maxus {

static const char *const UDS_TAG = "maxus_uds";

// big endian helpers (UDS uses big endian)
inline uint16_t be16(const std::vector<uint8_t> &d, size_t o) { return (uint16_t(d[o]) << 8) | d[o + 1]; }
inline uint32_t be24(const std::vector<uint8_t> &d, size_t o) {
  return (uint32_t(d[o]) << 16) | (uint32_t(d[o + 1]) << 8) | d[o + 2];
}

inline std::string hex(const std::vector<uint8_t> &d) {
  static const char *digits = "0123456789ABCDEF";
  std::string s;
  s.reserve(d.size() * 3);
  for (size_t i = 0; i < d.size(); i++) {
    if (i)
      s += ' ';
    s += digits[d[i] >> 4];
    s += digits[d[i] & 0x0F];
  }
  return s;
}

class UdsPoller {
 public:
  using Handler = std::function<void(uint16_t did, const std::vector<uint8_t> &data)>;

  void setup(esphome::canbus::Canbus *bus, uint32_t tx_id) {
    this->bus_ = bus;
    this->tx_id_ = tx_id;
  }
  bool is_setup() const { return this->bus_ != nullptr; }

  // Poll `did` every `interval_ms` while polling is allowed.
  void add(uint16_t did, uint32_t interval_ms) { this->items_.push_back({did, interval_ms, 0, false}); }

  void on_response(Handler h) { this->handler_ = std::move(h); }

  // Poll every DID again as soon as possible (e.g. after the vehicle woke up).
  void reset_schedule() {
    for (auto &it : this->items_)
      it.polled = false;
  }

  uint32_t timeouts() const { return this->timeouts_; }
  uint32_t responses() const { return this->responses_; }

  // Call periodically. `allowed` = false stops new requests (a running one completes).
  void loop(bool allowed) {
    if (this->bus_ == nullptr || this->items_.empty())
      return;
    const uint32_t now = esphome::millis();

    if (this->state_ != IDLE) {
      if (now - this->req_ms_ > RESPONSE_TIMEOUT_MS) {
        ESP_LOGW(UDS_TAG, "Timeout waiting for DID 0x%04X (got %u/%u bytes)", this->cur_did_,
                 (unsigned) this->buf_.size(), (unsigned) this->expected_);
        this->timeouts_++;
        this->state_ = IDLE;
        this->last_done_ms_ = now;
      }
      return;
    }

    if (!allowed || now - this->last_done_ms_ < REQUEST_GAP_MS)
      return;

    for (size_t n = 0; n < this->items_.size(); n++) {
      const size_t i = (this->next_ + n) % this->items_.size();
      auto &it = this->items_[i];
      if (!it.polled || now - it.last_ms >= it.interval_ms) {
        it.polled = true;
        it.last_ms = now;
        this->next_ = i + 1;
        this->request_(it.did);
        return;
      }
    }
  }

  // Feed every frame received on the ECU's response ID.
  void handle_frame(const std::vector<uint8_t> &x) {
    if (this->state_ == IDLE || x.empty())
      return;
    const uint32_t now = esphome::millis();
    const uint8_t pci = x[0] >> 4;

    if (pci == 0x0) {  // single frame
      const uint8_t len = x[0] & 0x0F;
      if (len == 0 || len > x.size() - 1)
        return;
      this->buf_.assign(x.begin() + 1, x.begin() + 1 + len);
      this->expected_ = len;
      this->finish_(now);
    } else if (pci == 0x1) {  // first frame
      if (x.size() < 8)
        return;
      this->expected_ = (uint16_t(x[0] & 0x0F) << 8) | x[1];
      this->buf_.assign(x.begin() + 2, x.end());
      this->seq_ = 1;
      this->state_ = WAIT_CF;
      this->req_ms_ = now;
      // flow control: continue to send, block size 0 (no further FC), STmin 0
      this->send_({0x30, 0x00, 0x00, PAD, PAD, PAD, PAD, PAD});
    } else if (pci == 0x2 && this->state_ == WAIT_CF) {  // consecutive frame
      if ((x[0] & 0x0F) != this->seq_) {
        ESP_LOGW(UDS_TAG, "ISO-TP sequence error for DID 0x%04X", this->cur_did_);
        this->state_ = IDLE;
        this->last_done_ms_ = now;
        return;
      }
      this->seq_ = (this->seq_ + 1) & 0x0F;
      const size_t n = std::min<size_t>(this->expected_ - this->buf_.size(), x.size() - 1);
      this->buf_.insert(this->buf_.end(), x.begin() + 1, x.begin() + 1 + n);
      this->req_ms_ = now;
      if (this->buf_.size() >= this->expected_)
        this->finish_(now);
    }
  }

 protected:
  static constexpr uint32_t RESPONSE_TIMEOUT_MS = 1000;
  static constexpr uint32_t REQUEST_GAP_MS = 50;
  static constexpr uint8_t PAD = 0xAA;

  enum State : uint8_t { IDLE, WAIT_SF_OR_FF, WAIT_CF };

  struct Item {
    uint16_t did;
    uint32_t interval_ms;
    uint32_t last_ms;
    bool polled;
  };

  void send_(const std::vector<uint8_t> &data) { this->bus_->send_data(this->tx_id_, false, data); }

  void request_(uint16_t did) {
    this->cur_did_ = did;
    this->buf_.clear();
    this->expected_ = 0;
    this->state_ = WAIT_SF_OR_FF;
    this->req_ms_ = esphome::millis();
    this->send_({0x03, 0x22, uint8_t(did >> 8), uint8_t(did & 0xFF), PAD, PAD, PAD, PAD});
  }

  void finish_(uint32_t now) {
    if (this->buf_.size() >= 3 && this->buf_[0] == 0x7F) {  // negative response
      if (this->buf_[2] == 0x78) {                          // response pending: keep waiting
        this->state_ = WAIT_SF_OR_FF;
        this->req_ms_ = now;
        return;
      }
      ESP_LOGW(UDS_TAG, "Negative response for DID 0x%04X: NRC 0x%02X", this->cur_did_, this->buf_[2]);
      this->state_ = IDLE;
      this->last_done_ms_ = now;
      return;
    }

    this->state_ = IDLE;
    this->last_done_ms_ = now;

    if (this->buf_.size() < 3 || this->buf_[0] != 0x62) {
      ESP_LOGW(UDS_TAG, "Unexpected response for DID 0x%04X: %s", this->cur_did_, hex(this->buf_).c_str());
      return;
    }
    const uint16_t did = be16(this->buf_, 1);
    if (did != this->cur_did_)
      ESP_LOGW(UDS_TAG, "Got DID 0x%04X while waiting for 0x%04X", did, this->cur_did_);

    this->responses_++;
    const std::vector<uint8_t> payload(this->buf_.begin() + 3, this->buf_.end());
    ESP_LOGD(UDS_TAG, "DID 0x%04X = %s", did, hex(payload).c_str());
    if (this->handler_)
      this->handler_(did, payload);
  }

  esphome::canbus::Canbus *bus_{nullptr};
  uint32_t tx_id_{0};
  std::vector<Item> items_;
  size_t next_{0};
  Handler handler_;

  State state_{IDLE};
  uint16_t cur_did_{0};
  uint32_t req_ms_{0};
  uint32_t last_done_ms_{0};
  std::vector<uint8_t> buf_;
  uint16_t expected_{0};
  uint8_t seq_{0};

  uint32_t timeouts_{0};
  uint32_t responses_{0};
};

inline UdsPoller uds;

}  // namespace maxus
