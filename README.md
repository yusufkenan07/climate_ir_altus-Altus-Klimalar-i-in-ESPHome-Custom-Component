[README.md](https://github.com/user-attachments/files/30525345/README.md)
# climate_ir_altus — Altus Klimalar için ESPHome Custom Component

> Altus (ve muhtemelen aynı OEM hattından gelen benzer LG-tabanlı) split klimaları, ESPHome + Home Assistant üzerinden IR ile kontrol etmek ve kumandadan gelen sinyalleri senkron olarak okumak için yazılmış custom climate component'i.

Bu doküman, hazır bir "kur ve kullan" rehberinden ziyade **süreci baştan sona anlatan bir teknik günlük** niteliğinde. Amaç sadece bu component'i paylaşmak değil, aynı yöntemi kendi klimanız için nasıl uygulayabileceğinizi de göstermek — çünkü Türkiye'de bu tür reverse-engineering çalışmalarının Türkçe kaynağı neredeyse yok.

---

## İçindekiler

1. [Neden bu proje var](#neden-bu-proje-var)
2. [Donanım gereksinimleri](#donanım-gereksinimleri)
3. [Protokolü çözme süreci](#protokolü-çözme-süreci)
4. [Protokol detayları](#protokol-detayları)
5. [Yazılım mimarisi](#yazılım-mimarisi)
6. [Kurulum](#kurulum)
7. [Bilinen sınırlamalar](#bilinen-sınırlamalar)
8. [Karşılaşılan hatalar ve çözümleri](#karşılaşılan-hatalar-ve-çözümleri)
9. [Aynı yöntemi kendi klimanıza uygulamak isterseniz](#aynı-yöntemi-kendi-klimanıza-uygulamak-isterseniz)
10. [Teşekkür / Referanslar](#teşekkür--referanslar)

---

## Neden bu proje var

ESPHome'un resmi component listesinde onlarca marka için IR klima desteği (`climate_ir_lg`, `climate_ir_fujitsu`, `climate_ir_mitsubishi` vb.) var, ama **Altus için hiçbir şey yok.** Türkiye'de yaygın kullanılan bu markanın, gerçekte hangi protokolü konuştuğuna dair hiçbir kamuya açık dokümantasyon bulunmuyor.

Bu projede yaptığımız şey:
- Altus'un fiziksel kumandasından ham IR sinyalini topladık
- Bit seviyesinde decode edip checksum algoritmasını, komut/fan/sıcaklık bit haritalarını **sıfırdan** çıkardık
- Bunu ESPHome'un `climate_ir` altyapısına uygun, gerçek dünyada test edilmiş bir component'e dönüştürdük

Sonuç: hem klimayı IR ile kontrol edebiliyoruz hem de fiziksel kumandadan gelen komutları decode edip Home Assistant'taki durumu senkron tutabiliyoruz.

### Test durumu

Bu component, **LG tabanlı bir Altus klimada test edilmiş ve çalışır durumdadır.**

Arçelik ve Beko grubuna ait, aynı şekilde **LG tabanlı anakart kullanan serilerde de çalışma ihtimali var** — ancak bu markalarda **denenmedi/doğrulanmadı.** Eğer Arçelik/Beko grubu bir klimanız varsa ve deneme fırsatınız olursa, sonucu paylaşmanız (issue açarak) başkalarına da yol gösterecektir.

---

## Donanım gereksinimleri

- Bir ESP32 (veya ESP8266) geliştirme kartı
- IR LED (gönderim için) — bir NPN transistör üzerinden sürülmesi önerilir
- IR alıcı modülü (TSOP38xx serisi gibi) — kumandadan sinyal okumak için
- Klimanın görüş hattında (line-of-sight) bir konumlandırma

```yaml
remote_transmitter:
  pin: GPIO4
  carrier_duty_percent: 50%

remote_receiver:
  pin: GPIO14
  dump: all   # ilk kurulum/analiz aşamasında faydalı
```

### Alternatif kurulum: Doğrudan enjeksiyon (ileri seviye, kendi sorumluluğunuzda)

Yukarıdaki yöntem "harici IR blaster" mantığıyla çalışır — ESP, IR LED üzerinden havaya sinyal yayar, klimanın kendi IR alıcısı bunu normal kumandaymış gibi algılar. Bu, en yaygın ve en güvenli yöntemdir.

Bu projede biz farklı bir yol izledik: **IR LED/transistör devresi kullanmadan, ESP'nin sinyalini bir logic level shifter üzerinden doğrudan klimanın kendi IR alıcı modülünün sinyal hattına enjekte ettik.** Yani havadan IR göndermek yerine, klimanın iç devresine kablo ile bağlanıp "sanki kumandadan sinyal geliyormuş gibi" doğrudan elektriksel sinyal veriyoruz.

**Bu yöntemin avantajı:** Ekstra IR LED/transistör devresi kurmaya gerek kalmıyor, tek bir level shifter yeterli.

**Riskleri ve neden "ileri seviye" dediğimiz:**
- Klimanın iç devresine fiziksel/elektriksel müdahale gerektirir — yanlış voltaj seviyesi veya yanlış pin bağlantısı, IR alıcı modülünü veya ana kartı zarar verebilir.
- Klimanın garantisini etkileyebilir (üniteyi açmayı gerektirir).
- IR alıcı modülünün gerçek lojik seviyesini (3.3V/5V) ve sinyal pinini doğru tespit etmeniz gerekir — model modelden farklılık gösterebilir.

Bu yöntemi yalnızca donanım/elektronik konusunda deneyimliyseniz ve riski kabul ediyorsanız deneyin. Standart harici IR blaster yöntemi, çoğu kullanıcı için yeterli ve çok daha güvenlidir.

---

## Protokolü çözme süreci

Referans alacağımız hazır bir Altus dokümantasyonu olmadığı için, **LG'nin ESPHome component'ini (`climate_ir_lg`) iskelet olarak kullandık** — çünkü Altus'un donanımsal olarak LG tabanlı bir devre kullandığı, kumandanın farklı bir encoding kullandığı biliniyordu.

### 1. Ham veri toplama

`remote_receiver`'da `dump: all` açarak, kumandadan her tuş için ham mark/space (µs) dizisini topladık — power on/off, sıcaklık artır/azalt (16-32°C), mod değişimleri (Cool/Heat/Dry/Fan/AI), fan hızları, swing, turbo, sleep gibi mümkün olduğunca çok senaryo için.

```
Received Raw: 3149, -9692, 654, -1406, 652, -363, ...
```

### 2. Header ve bit yapısını tespit etme

İlk mark/space çifti **header** (senkronizasyon darbesi). Sonraki her mark/space çifti bir **bit**'i temsil ediyor:
- Sabit bir mark süresi (IR LED'in "yanık" kaldığı süre)
- Değişken bir space süresi — **kısa space = bit 0, uzun space = bit 1**

Gerçek veride mark süresi oldukça dalgalanabiliyor (gürültü, multipath yansıma), ama space süresi net iki gruba ayrılıyor ve aradaki boşluk hiç dolmuyor. Bu yüzden decode'u **sadece space süresine bakan bir threshold** üzerine kurduk — mark'taki gürültüyü tamamen göz ardı ederek, çok daha dayanıklı bir çözüm elde ettik.

### 3. Checksum'ı bulma

Paketin son 4 biti (nibble) bir checksum. Elle ve script ile birçok gerçek paketi analiz ederek şu formülü doğruladık:

```cpp
uint8_t calc_checksum(uint32_t value) {
  uint8_t sum = 0;
  for (uint8_t i = 1; i < 8; i++) {
    sum += (value >> (i * 4)) & 0xF;   // paketin diğer 7 nibble'ının toplamı
  }
  return sum & 0xF;
}
```

Yani checksum, paketin kendisi hariç diğer tüm 4-bit gruplarının toplamının alt 4 biti. Bu formülü **19 farklı gerçek kayıtla test ettik, hepsinde birebir tuttu.**

### 4. Komut/fan/sıcaklık alanlarını haritalama

Farklı senaryolarda (örneğin "24°C Cool Fan-Auto Power-On" vs "Power-Off") elde edilen paketleri karşılaştırarak, hangi bit aralığının neyi kodladığını çıkardık:

| Bit aralığı | Anlamı |
|---|---|
| `0xFF00000` (üst 8 bit) | Sabit header — `0x8800000` |
| `0xFF000` | Komut (mod + power-on/continuation ayrımı) |
| `0xF00` | Sıcaklık (`gerçek_derece = ham_değer + 15`) |
| `0xF0` | Fan hızı |
| `0xF` | Checksum |

---

## Protokol detayları

### Timing

| Parametre | Değer |
|---|---|
| Header mark | ~8000µs |
| Header space | ~4000µs |
| Bit mark | ~600µs |
| Bit "1" space | ~1600µs |
| Bit "0" space | ~550µs |
| Paket uzunluğu | 28 bit |

### Komut tablosu

| Komut | Değer | Açıklama |
|---|---|---|
| `COMMAND_ON_COOL` | `0x00000` | Kapalıyken Cool moduna geçiş (power-on tetikleyici) |
| `COMMAND_COOL` | `0x08000` | Zaten açıkken Cool modunda kal/güncelle |
| `COMMAND_ON_HEAT` / `COMMAND_HEAT` | `0x04000` / `0x0C000` | Heat modu (on/continuation) |
| `COMMAND_ON_DRY` / `COMMAND_DRY` | `0x01000` / `0x09000` | Dry modu |
| `COMMAND_ON_FAN` / `COMMAND_FAN` | `0x02000` / `0x0A000` | Fan-only modu |
| `COMMAND_ON_AI` / `COMMAND_AI` | `0x03000` / `0x0B000` | Auto (AI) modu |
| `COMMAND_OFF` | `0xC0000` | Kapatma |
| `COMMAND_SWING` | `0x10000` | Swing aç/kapat tetikleyici (toggle — cihaz kendi içinde çeviriyor) |

> **Önemli detay:** Altus'ta "power-on" ve "zaten açık, mod değiştir" için **farklı komut kodları** var. Bu ayrımı doğru yönetmezseniz (bkz. [Karşılaşılan hatalar](#karşılaşılan-hatalar-ve-çözümleri)), klima açık değilken "aç" komutu gönderildiğinde hiçbir etki olmaz.

### Fan tablosu

| Fan | Değer |
|---|---|
| Low | `0x00` |
| Medium | `0x20` |
| High | `0x40` |
| Auto | `0x50` |

### Sıcaklık

```
gerçek_derece = ham_değer(4 bit) + 15
```

Aralık: 18-30°C (klimanın desteklediği fiziksel sınır).

---

## Yazılım mimarisi

```
/esphome/custom_components/climate_ir_altus/
├── __init__.py           # Python paket tanımlayıcı (kasıtlı olarak boş)
├── climate.py            # ESPHome config şeması (CODEOWNERS, AUTO_LOAD, YAML seçenekleri)
├── climate_ir_altus.h     # Sınıf tanımı, state alanları
└── climate_ir_altus.cpp   # Encode (transmit_state) + Decode (on_receive) + checksum mantığı
```

- **`transmit_state()`** — Home Assistant'tan gelen komutu (mod/fan/sıcaklık/swing) 28-bit pakete çevirir, checksum ekler, IR olarak gönderir.
- **`on_receive()`** — Kumandadan gelen ham IR verisini header+threshold ile decode eder, checksum'ı doğrular, geçerliyse HA'daki climate entity'sini günceller.

---

## Kurulum

Aşağıda uçtan uca çalışan, tam bir örnek config bulunuyor. Kendi cihazınıza göre `name`, `wifi`, `pin` gibi alanları düzenlemeniz yeterli.

```yaml
esphome:
  name: altus-klima

esp32:
  board: esp32dev   # kendi ESP32 kartınıza göre değiştirin

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
logger:

# custom_components klasörünü tanıt (bkz. aşağıdaki klasör yapısı notu)
external_components:
  - source:
      type: local
      path: custom_components

# IR gönderim (klimaya komut gönderirken kullanılır)
remote_transmitter:
  pin: GPIO4
  carrier_duty_percent: 50%

# IR alım (kumandadan gelen sinyalleri okumak için)
remote_receiver:
  pin: GPIO14
  dump: all   # ilk kurulum/analiz aşamasında faydalı, sorun yoksa sonra kaldırılabilir

climate:
  - platform: climate_ir_altus
    name: "Salon Klima"
    # Timing ayarları opsiyonel — belirtilmezse aşağıdaki varsayılanlar kullanılır
    header_high: 8000us
    header_low: 4000us
    bit_high: 600us
    bit_one_low: 1600us
    bit_zero_low: 550us
```

> `!secret wifi_ssid` / `!secret wifi_password` kullanımı, gerçek WiFi bilgilerinizi ayrı bir `secrets.yaml` dosyasında tutmanızı sağlar — bu dosyayı asla GitHub'a yüklemeyin (bkz. `.gitignore`).

`/esphome/custom_components/climate_ir_altus/` klasörünü, YAML config dosyanızın bulunduğu dizinin altına (`esphome/custom_components/...`) aynen kopyalamanız yeterli — ESPHome bu isimlendirmeyi otomatik tanır.

> **Not:** `custom_components` klasörü ESPHome kurulumunuzda varsayılan olarak **bulunmayabilir** — bu tamamen sizin oluşturmanız gereken bir klasördür (stok gelmiyor). Yoksa `esphome/` dizininizin altına elle `custom_components` adında bir klasör açıp, içine `climate_ir_altus` alt klasörünü ve dosyaları yerleştirmeniz gerekiyor.

---

## Bilinen sınırlamalar

- **Heat / Dry modları** komut kodları, Cool/Fan ile aynı numaralandırma mantığını takip ettiği için yüksek ihtimalle doğru, ama gürültülü capture'lar yüzünden **checksum ile birebir doğrulanamadı.** Kullanırken bir sorun görürseniz issue açın.
- **Turbo / Sleep** fonksiyonları keşfedildi (checksum tutan gerçek paketler analiz edildi) ama component'e henüz **entegre edilmedi.**
- **AI (Auto/HEAT_COOL) modunda sıcaklık** gönderilmiyor — bu, LG'nin kendi resmi ESPHome component'inde de bilinen bir davranış/kısıtlama.

---

## Karşılaşılan hatalar ve çözümleri

Bu bölümü özellikle bırakıyorum çünkü aynı hataya düşebilecek başkalarına faydalı olabilir.

### "Kumandayla kapatınca HA güncelleniyor ama HA'dan tekrar açınca hiçbir şey olmuyor"

**Kök neden:** `mode_before_` adlı bir dahili değişken, "klima açılırken mi yoksa zaten açıkken mi komut gönderiyoruz" ayrımını yapmak için kullanılıyor. Bu değişken sadece **HA'dan komut gönderildiğinde** güncelleniyordu — kumandadan gelen bir "kapat" sinyali `on_receive()` içinde `this->mode`'u günceller ama `mode_before_`'u güncellemezdi. Sonuç: HA "aç" dediğinde, kod hâlâ "zaten açıktı" sanıp yanlış komut varyantını (power-on tetikleyici yerine continuation komutu) gönderiyordu — klima kapalıyken bu komutun hiçbir etkisi olmuyor.

**Çözüm:** `on_receive()` içinde mod decode edilen her yerde `mode_before_`'u da güncelledik, böylece bu değişken "HA'nın son gönderdiği" değil "klimanın gerçekten son bilinen durumu"nu yansıtıyor.

### Header'ı "bypass" ederken imleci ilerletmeyi unutmak

`expect_item()` çağrısını sadece yorum satırına alıp header kontrolünü devre dışı bırakmak, aynı zamanda header örneklerinin **atlanmasını (consume edilmesini)** da iptal ediyor — çünkü bu fonksiyon hem kontrol eder hem imleci ilerletir. Sonuç: bit okuma header verisinden başlıyor, ilk bitte anında decode hatası. Header'ı manuel `advance()` ile atlamak gerekiyor.

---

## Aynı yöntemi kendi klimanıza uygulamak isterseniz

1. `remote_receiver`'da `dump: all` açıp kumandanızdan mümkün olduğunca çok senaryo için ham veri toplayın.
2. Header'ı (ilk mark/space çifti) ve bit sayısını tespit edin.
3. Farklı senaryoları karşılaştırarak checksum, komut/fan/sıcaklık bit alanlarını çıkarın (basit bir Python scripti ile checksum formülünü otomatik test etmek çok işinizi kolaylaştırır).
4. Eğer klimanız başka bir markanın (LG, Gree, Fujitsu vb.) alt yapısını kullanıyorsa, o markanın ESPHome/IRremoteESP8266 component'ini iskelet olarak kullanın — muhtemelen timing sabitleri ve genel yapı çok yakın çıkacaktır.

---

## Teşekkür / Referanslar

- [ESPHome `climate_ir_lg`](https://github.com/esphome/esphome/tree/dev/esphome/components/climate_ir_lg) — genel yapı ve timing sabitleri için iskelet olarak kullanıldı.
- [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266) — AUX-tabanlı klimalar (Baymak dahil) için referans protokol dokümantasyonu barındıran, kapsamlı açık kaynak IR kütüphanesi.

---

## Lisans

Bu proje MIT lisansı ile paylaşılmaktadır. Kod, referans alınan açık kaynak projelerin (LGPL-3.0) telif bildirimlerine saygı gösterilerek hazırlanmıştır.
