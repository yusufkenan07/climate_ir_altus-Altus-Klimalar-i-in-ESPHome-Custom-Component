#pragma once

#include "esphome/components/climate_ir/climate_ir.h"

#include <cinttypes>

namespace esphome::climate_ir_altus {

// Desteklenen sıcaklık aralığı
const uint8_t TEMP_MIN = 18;
const uint8_t TEMP_MAX = 30;

class AltusIrClimate final : public climate_ir::ClimateIR {
 public:
  AltusIrClimate()
      : climate_ir::ClimateIR(
            TEMP_MIN,
            TEMP_MAX,
            1.0f,
            true,   // supports cool
            true,   // supports heat
            {
                climate::CLIMATE_FAN_AUTO,
                climate::CLIMATE_FAN_LOW,
                climate::CLIMATE_FAN_MEDIUM,
                climate::CLIMATE_FAN_HIGH,
            },
            {
                climate::CLIMATE_SWING_OFF,
                climate::CLIMATE_SWING_VERTICAL,
            }) {}

  void control(const climate::ClimateCall &call) override {
    this->send_swing_cmd_ = call.get_swing_mode().has_value();

    // Klima kapatıldığında swing sıfırlansın
    auto mode = call.get_mode();
    if (mode.has_value() && *mode == climate::CLIMATE_MODE_OFF)
      this->swing_mode = climate::CLIMATE_SWING_OFF;

    climate_ir::ClimateIR::control(call);
  }

  // Timing ayarları
  void set_header_high(uint32_t value) { header_high_ = value; }
  void set_header_low(uint32_t value) { header_low_ = value; }

  void set_bit_high(uint32_t value) { bit_high_ = value; }
  void set_bit_one_low(uint32_t value) { bit_one_low_ = value; }
  void set_bit_zero_low(uint32_t value) { bit_zero_low_ = value; }

 protected:
  /// HA → Klima
  void transmit_state() override;

  /// Kumanda → ESP
  bool on_receive(remote_base::RemoteReceiveData data) override;

  // Altus checksum
  void calc_checksum_(uint32_t &value);

  // Ham IR gönderimi
  void transmit_(uint32_t value);

  // IR zamanlamaları
  uint32_t header_high_;
  uint32_t header_low_;

  uint32_t bit_high_;
  uint32_t bit_one_low_;
  uint32_t bit_zero_low_;

  bool send_swing_cmd_{false};

  climate::ClimateMode mode_before_{climate::CLIMATE_MODE_OFF};

  // ---------- V2 için hazırlık ----------

  /// Kumandadan gelen veri doğrulandı mı?
  bool sync_lock_{false};

  /// Son geçerli Altus paketi
  uint32_t last_packet_{0};

  /// Son decode edilen sıcaklık
  uint8_t last_temperature_{24};

  /// Son decode edilen fan
  climate::ClimateFanMode last_fan_{climate::CLIMATE_FAN_AUTO};

  /// Son decode edilen mod
  climate::ClimateMode last_mode_{climate::CLIMATE_MODE_OFF};
};

}  // namespace esphome::climate_ir_altus