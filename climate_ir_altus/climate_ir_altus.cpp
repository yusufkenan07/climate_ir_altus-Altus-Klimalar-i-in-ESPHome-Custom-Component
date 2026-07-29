#include "climate_ir_altus.h"
#include "esphome/core/log.h"

namespace esphome::climate_ir_altus {

static const char *const TAG = "climate.climate_ir_altus";

//------------------------------------------------------
// Komut Kodları
//------------------------------------------------------

const uint32_t COMMAND_MASK      = 0xFF000;
const uint32_t COMMAND_OFF       = 0xC0000;
const uint32_t COMMAND_SWING     = 0x10000;
const uint32_t PACKET_PREFIX = 0x8800000;

const uint32_t COMMAND_ON_COOL   = 0x00000;
const uint32_t COMMAND_ON_DRY    = 0x01000;
const uint32_t COMMAND_ON_FAN    = 0x02000;
const uint32_t COMMAND_ON_AI     = 0x03000;
const uint32_t COMMAND_ON_HEAT   = 0x04000;

const uint32_t COMMAND_COOL      = 0x08000;
const uint32_t COMMAND_DRY       = 0x09000;
const uint32_t COMMAND_FAN       = 0x0A000;
const uint32_t COMMAND_AI        = 0x0B000;
const uint32_t COMMAND_HEAT      = 0x0C000;

//------------------------------------------------------
// Fan Kodları
//------------------------------------------------------

const uint32_t FAN_MASK = 0xF0;

const uint32_t FAN_AUTO = 0x50;
const uint32_t FAN_LOW  = 0x00;
const uint32_t FAN_MED  = 0x20;
const uint32_t FAN_HIGH = 0x40;

//------------------------------------------------------
// Sıcaklık
//------------------------------------------------------

const uint8_t TEMP_OFFSET = 15;
const uint32_t TEMP_MASK  = 0xF00;
const uint32_t TEMP_SHIFT = 8;

const uint32_t CHECKSUM_MASK = 0xFUL;

const uint16_t BITS = 28;

//------------------------------------------------------
// Checksum yardımcıları
//------------------------------------------------------

// Paketin checksum'ı hariç tüm nibble'larının (4-bit) toplamını hesaplar.
static uint8_t calc_checksum(uint32_t value) {
  uint8_t sum = 0;

  for (uint8_t i = 1; i < 8; i++) {
    sum += (value >> (i * 4)) & CHECKSUM_MASK;
  }

  return sum & CHECKSUM_MASK;
}

// Alınan paketin checksum nibble'ının (bit 0-3) doğru olup olmadığını kontrol eder.
static bool verify_checksum(uint32_t value) {
  return (value & CHECKSUM_MASK) == calc_checksum(value);
}

//------------------------------------------------------
// Transmit State
//------------------------------------------------------

void AltusIrClimate::transmit_state() {

  //----------------------------------------------------
  // Sync koruması
  //----------------------------------------------------

//  if (!this->sync_lock_) {
//    ESP_LOGW(TAG, "SYNC LOCK = FALSE -> IR transmission skipped.");
//    this->publish_state();
//    return;
//  }

  //----------------------------------------------------
  // Paket başlangıcı
  //----------------------------------------------------

  uint32_t remote_state = PACKET_PREFIX;

  //----------------------------------------------------
  // Swing komutu
  //----------------------------------------------------

  if (this->send_swing_cmd_) {

    this->send_swing_cmd_ = false;
    remote_state |= COMMAND_SWING;

  } else {

    bool power_on =
        (this->mode_before_ == climate::CLIMATE_MODE_OFF);

    switch (this->mode) {

      case climate::CLIMATE_MODE_COOL:
        remote_state |= power_on ? COMMAND_ON_COOL : COMMAND_COOL;
        break;

      case climate::CLIMATE_MODE_HEAT:
        remote_state |= power_on ? COMMAND_ON_HEAT : COMMAND_HEAT;
        break;

      case climate::CLIMATE_MODE_DRY:
        remote_state |= power_on ? COMMAND_ON_DRY : COMMAND_DRY;
        break;

      case climate::CLIMATE_MODE_FAN_ONLY:
        remote_state |= power_on ? COMMAND_ON_FAN : COMMAND_FAN;
        break;

      case climate::CLIMATE_MODE_HEAT_COOL:
        remote_state |= power_on ? COMMAND_ON_AI : COMMAND_AI;
        break;

      case climate::CLIMATE_MODE_OFF:
      default:
        remote_state |= COMMAND_OFF;
        break;
    }
  }

  //----------------------------------------------------
  // Son modu kaydet
  //----------------------------------------------------

  this->mode_before_ = this->mode;

  ESP_LOGD(TAG, "Mode = %d", this->mode);

  //----------------------------------------------------
  // Fan
  //----------------------------------------------------

  if (this->mode == climate::CLIMATE_MODE_OFF) {

    remote_state |= FAN_AUTO;

  } else {

    switch (this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO)) {

      case climate::CLIMATE_FAN_LOW:
        remote_state |= FAN_LOW;
        break;

      case climate::CLIMATE_FAN_MEDIUM:
        remote_state |= FAN_MED;
        break;

      case climate::CLIMATE_FAN_HIGH:
        remote_state |= FAN_HIGH;
        break;

      case climate::CLIMATE_FAN_AUTO:
      default:
        remote_state |= FAN_AUTO;
        break;
    }
  }

  //----------------------------------------------------
  // Sıcaklık
  //----------------------------------------------------

  if (this->mode == climate::CLIMATE_MODE_COOL ||
      this->mode == climate::CLIMATE_MODE_HEAT) {

    uint8_t temp =
        (uint8_t) roundf(
            clamp<float>(
                this->target_temperature,
                TEMP_MIN,
                TEMP_MAX));

    remote_state |= ((temp - TEMP_OFFSET) << TEMP_SHIFT);
  }

  //----------------------------------------------------
  // Son paketi sakla
  //----------------------------------------------------

  this->last_packet_ = remote_state;

  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "ALTUS RX");
  ESP_LOGI(TAG, "Packet : 0x%08" PRIX32, remote_state);
  ESP_LOGI(TAG, "========================================");

  //----------------------------------------------------
  // Gönder
  //----------------------------------------------------

  this->transmit_(remote_state);

  this->publish_state();
}

//------------------------------------------------------
// Receive / Decode
//------------------------------------------------------

// Bir "1" ile "0" bitini ayıran space eşiği (mikrosaniye).
// Gerçek kumandadan alınan verilerde:
//   bit=0 space aralığı  ~250-520us
//   bit=1 space aralığı  ~1140-1540us
// Aradaki boşluk hiç dolmuyor, bu yüzden sabit bir eşik güvenilir.
static constexpr int32_t BIT_THRESHOLD_US = 900;

// Header mark/space için gözlemlenen gerçek aralık + pay.
static constexpr int32_t HEADER_MARK_MIN  = 2500;
static constexpr int32_t HEADER_MARK_MAX  = 4200;
static constexpr int32_t HEADER_SPACE_MIN = -11000;
static constexpr int32_t HEADER_SPACE_MAX = -8000;

bool AltusIrClimate::on_receive(remote_base::RemoteReceiveData data) {

  uint32_t remote_state = 0;

  ESP_LOGI(TAG, "==============================");
  ESP_LOGI(TAG, "ALTUS RX START");

  //----------------------------------------------------
  // Header kontrolü (genişletilmiş tolerans) — ÖNEMLİ:
  // header burada kontrol EDİLİP AYNI ZAMANDA imleç
  // ilerletiliyor (advance). Sadece kontrolü atlayıp
  // advance çağırmazsan bit okuma header verisinden
  // başlar ve ilk bitte decode başarısız olur.
  //----------------------------------------------------

  if (!data.is_valid(1)) {
    ESP_LOGV(TAG, "Not enough data for header.");
    return false;
  }

  int32_t header_mark = data.peek(0);
  int32_t header_space = data.peek(1);

  if (header_mark < HEADER_MARK_MIN || header_mark > HEADER_MARK_MAX ||
      header_space < HEADER_SPACE_MIN || header_space > HEADER_SPACE_MAX) {

    ESP_LOGV(TAG, "Header mismatch: mark=%d space=%d", header_mark, header_space);
    return false;
  }

  data.advance(2);

  ESP_LOGI(TAG, "HEADER OK");

  //----------------------------------------------------
  // Veri Oku — sadece space süresine göre (mark'taki
  // gürültü/dalgalanma göz ardı edilir)
  //----------------------------------------------------

  for (uint8_t nbits = 0; nbits < BITS; nbits++) {

    if (!data.is_valid(1)) {
      ESP_LOGV(TAG, "Decode failed: not enough data at bit %u", nbits);
      return false;
    }

    int32_t mark = data.peek(0);
    int32_t space = data.peek(1);

    if (mark <= 0) {
      ESP_LOGV(TAG, "Decode failed: invalid mark at bit %u", nbits);
      return false;
    }

    remote_state <<= 1;

    if (space <= -BIT_THRESHOLD_US) {
      remote_state |= 1;
    }

    data.advance(2);
  }

  ESP_LOGD(TAG, "Decoded Packet = 0x%08" PRIX32, remote_state);

  //----------------------------------------------------
  // Paket Başlığı
  //----------------------------------------------------

  if ((remote_state & 0xFF00000) != PACKET_PREFIX) {

    ESP_LOGW(TAG, "Invalid packet header.");
    return false;
  }

  //----------------------------------------------------
  // Checksum kontrolü
  //----------------------------------------------------

  if (!verify_checksum(remote_state)) {

    ESP_LOGW(TAG, "Checksum mismatch, packet dropped.");
    return false;
  }

  //----------------------------------------------------
  // Sync tamam
  //----------------------------------------------------

  this->sync_lock_ = true;
  this->last_packet_ = remote_state;

  //----------------------------------------------------
  // Power / Komut
  //----------------------------------------------------

  if ((remote_state & COMMAND_MASK) == COMMAND_OFF) {

    this->mode = climate::CLIMATE_MODE_OFF;
    this->swing_mode = climate::CLIMATE_SWING_OFF;

    // FIX: mode_before_ sadece transmit_state() içinde güncelleniyordu.
    // Kumandadan gelen OFF komutu burada da senkron edilmezse,
    // sonraki HA'dan "aç" isteğinde power_on yanlış hesaplanıp
    // "zaten açık" komutu (COMMAND_COOL vb.) gönderiliyor, klima
    // gerçekte kapalı olduğu için hiçbir etkisi olmuyordu.
    this->mode_before_ = climate::CLIMATE_MODE_OFF;

  } else if ((remote_state & COMMAND_MASK) == COMMAND_SWING) {

    this->swing_mode =
        this->swing_mode == climate::CLIMATE_SWING_OFF
            ? climate::CLIMATE_SWING_VERTICAL
            : climate::CLIMATE_SWING_OFF;

  } else {

    switch (remote_state & COMMAND_MASK) {

      case COMMAND_ON_HEAT:
      case COMMAND_HEAT:
        this->mode = climate::CLIMATE_MODE_HEAT;
        break;

      case COMMAND_ON_DRY:
      case COMMAND_DRY:
        this->mode = climate::CLIMATE_MODE_DRY;
        break;

      case COMMAND_ON_FAN:
      case COMMAND_FAN:
        this->mode = climate::CLIMATE_MODE_FAN_ONLY;
        break;

      case COMMAND_ON_AI:
      case COMMAND_AI:
        this->mode = climate::CLIMATE_MODE_HEAT_COOL;
        break;

      case COMMAND_ON_COOL:
      case COMMAND_COOL:
      default:
        this->mode = climate::CLIMATE_MODE_COOL;
        break;
    }

    //--------------------------------------------------
    // Fan
    //--------------------------------------------------

    switch (remote_state & FAN_MASK) {

      case FAN_LOW:
        this->fan_mode = climate::CLIMATE_FAN_LOW;
        break;

      case FAN_MED:
        this->fan_mode = climate::CLIMATE_FAN_MEDIUM;
        break;

      case FAN_HIGH:
        this->fan_mode = climate::CLIMATE_FAN_HIGH;
        break;

      case FAN_AUTO:
      default:
        this->fan_mode = climate::CLIMATE_FAN_AUTO;
        break;
    }

    //--------------------------------------------------
    // Temperature
    //--------------------------------------------------

    if (this->mode == climate::CLIMATE_MODE_COOL ||
        this->mode == climate::CLIMATE_MODE_HEAT) {

      this->target_temperature =
          ((remote_state & TEMP_MASK) >> TEMP_SHIFT) + TEMP_OFFSET;

      this->last_temperature_ =
          static_cast<uint8_t>(this->target_temperature);
    }

    //--------------------------------------------------
    // Son Durumları Sakla
    //--------------------------------------------------

    this->last_mode_ = this->mode;

    // FIX: Aynı sebep — kumandadan gelen herhangi bir "açık" mod
    // (Cool/Heat/Dry/Fan/AI) decode edildiğinde de mode_before_'u
    // gerçek duruma senkronla, yoksa bir sonraki HA transmit'inde
    // power_on hesabı yanlış (stale) veriye dayanır.
    this->mode_before_ = this->mode;

    this->last_fan_ =
        this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO);
  }

  ESP_LOGD(TAG,
           "SYNC LOCK OK | Mode=%d Fan=%d Temp=%.1f",
           (int) this->mode,
           (int) this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO),
           this->target_temperature);

  this->publish_state();

  return true;
}

//------------------------------------------------------
// IR Transmit
//------------------------------------------------------

void AltusIrClimate::transmit_(uint32_t value) {

  this->calc_checksum_(value);

  ESP_LOGD(TAG, "Sending Altus packet : 0x%08" PRIX32, value);

  auto transmit = this->transmitter_->transmit();
  auto *data = transmit.get_data();

  data->set_carrier_frequency(38000);

  data->reserve(2 + (BITS * 2u));

  data->item(this->header_high_, this->header_low_);

  for (uint32_t mask = (1UL << (BITS - 1)); mask != 0; mask >>= 1) {

    if (value & mask) {

      data->item(
          this->bit_high_,
          this->bit_one_low_);

    } else {

      data->item(
          this->bit_high_,
          this->bit_zero_low_);
    }
  }

  data->mark(this->bit_high_);

  transmit.perform();
}

//------------------------------------------------------
// Checksum
//------------------------------------------------------

void AltusIrClimate::calc_checksum_(uint32_t &value) {

  value &= ~CHECKSUM_MASK;
  value |= calc_checksum(value);
}

}  // namespace esphome::climate_ir_altus
