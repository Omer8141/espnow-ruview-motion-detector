# ESP32-S3 ESP-NOW hareket dedektörü — sıfırdan kullanım kılavuzu

Bu kılavuz, elinde yalnızca iki ESP32-S3 kart ve proje dosyaları olduğu durumu anlatır. Kartlara firmware yüklenmeden gerçek sensör verisi oluşmaz. Frontend tek başına sadece **Demo** modunda çalışır.

## Sistem nasıl çalışacak?

- **Kart A — Alıcı (RX):** ESP-NOW paketlerini ve CSI verisini alır, özellikleri çıkarır ve USB üzerinden bilgisayara JSON gönderir.
- **Kart B — Verici (TX):** Alıcının MAC adresine saniyede 20 ESP-NOW paketi gönderir.
- **Frontend:** Kart A'nın USB seri çıktısını Chrome veya Edge ile açar ve grafikleri gösterir.
- **Eğitilmiş model:** Son aşamada Kart A üzerinde `empty`, `still_person` ve `moving_person` kararı verir.

Kartlar arasında jumper kablo gerekmez. İki kartın da sadece USB ile beslenmesi yeterlidir.

## Gerekenler

- 2 adet ESP32-S3 geliştirme kartı
- En az 1 adet veri aktarabilen USB kablosu
- İkinci kartı aynı anda beslemek için ikinci USB kablosu veya USB adaptörü
- Windows bilgisayar
- Visual Studio Code
- VS Code için resmi PlatformIO IDE eklentisi
- Frontend için Node.js 22 veya daha yeni sürüm
- Masaüstü Google Chrome veya Microsoft Edge

> Proje şu anda `esp32-s3-devkitc-1` kart tanımını kullanıyor. Kartın üzerinde `ESP32-S3-DevKitC-1` veya `ESP32-S3-WROOM-1` benzeri ne yazdığını ilk önce doğrula. Farklı bir kartsa `platformio.ini` içindeki `board` satırının değiştirilmesi gerekebilir.

## Aşama 1 — PlatformIO'yu kur

1. Visual Studio Code'u kur.
2. VS Code içinde **Extensions** bölümünü aç.
3. `PlatformIO IDE` ara ve resmi eklentiyi kur.
4. Kurulum bitince VS Code'u yeniden başlat.
5. VS Code ile şu klasörü aç:

```text
C:\Users\PC\Documents\Codex\2026-08-10\2\outputs\espnow-ruview-motion-detector
```

PlatformIO IDE kendi komut satırını içerir; ayrıca bağımsız PlatformIO kurmak gerekmez. Aşağıdaki `pio` komutlarını VS Code içindeki **PlatformIO Core CLI** terminalinde çalıştır.

## Aşama 2 — Kartları isimlendir

Karışmaması için kartların üzerine geçici etiket yapıştır:

- Birinci kart: `A - RX`
- İkinci kart: `B - TX`

İlk işlemlerde bilgisayara aynı anda yalnızca üzerinde çalıştığın kartı bağla.

## Aşama 3 — Kart A'ya alıcı firmware'ini ilk kez yükle

1. Sadece `A - RX` kartını USB ile bilgisayara bağla.
2. PlatformIO terminalinde şu komutları çalıştır:

```powershell
cd "C:\Users\PC\Documents\Codex\2026-08-10\2\outputs\espnow-ruview-motion-detector\firmware\receiver"
pio run -t upload
```

3. Yükleme tamamlanınca seri monitörü aç:

```powershell
pio device monitor -b 921600
```

4. Gerekirse kartın `EN` veya `RESET` düğmesine bir kez bas.
5. Ekranda şu biçimde bir satır göreceksin:

```text
Receiver STA MAC: AA:BB:CC:DD:EE:FF
```

6. Bu adresi aynen not et. Örnek adresi kullanma; her kartın adresi farklıdır.
7. Seri monitörü `Ctrl+C` ile kapat.

Bu ilk yüklemede `TRANSMITTER_MAC` hâlâ sıfır olduğu için hata mesajı normaldir. Bu adımın amacı Kart A'nın gerçek MAC adresini öğrenmektir.

## Aşama 4 — Kart A'nın MAC adresini verici ayarına yaz

Şu dosyayı aç:

```text
firmware\transmitter\include\peer_config.h
```

Bu satırı bul:

```cpp
static constexpr uint8_t RECEIVER_MAC[6] = {0, 0, 0, 0, 0, 0};
```

Örneğin alıcının adresi `A0:B7:65:12:34:56` ise satırı şöyle değiştir:

```cpp
static constexpr uint8_t RECEIVER_MAC[6] = {
    0xA0, 0xB7, 0x65, 0x12, 0x34, 0x56
};
```

Dosyayı kaydet. `ESPNOW_CHANNEL = 6` satırına şimdilik dokunma.

## Aşama 5 — Kart B'ye verici firmware'ini yükle

1. Kart A'yı bilgisayardan çıkar.
2. `B - TX` kartını USB ile bağla.
3. PlatformIO terminalinde çalıştır:

```powershell
cd "C:\Users\PC\Documents\Codex\2026-08-10\2\outputs\espnow-ruview-motion-detector\firmware\transmitter"
pio run -t upload
pio device monitor -b 115200
```

4. Gerekirse `EN` veya `RESET` düğmesine bas.
5. Şu satırı bul ve vericinin MAC adresini not et:

```text
Transmitter STA MAC: 11:22:33:44:55:66
```

6. Seri monitörü `Ctrl+C` ile kapat.

Alıcı o anda kapalı olduğu için `tx_failed` artabilir. Bu aşamada normaldir.

## Aşama 6 — Kart B'nin MAC adresini alıcı ayarına yaz

Şu dosyayı aç:

```text
firmware\receiver\include\peer_config.h
```

Bu satırı bul:

```cpp
static constexpr uint8_t TRANSMITTER_MAC[6] = {0, 0, 0, 0, 0, 0};
```

Örneğin vericinin adresi `11:22:33:44:55:66` ise şöyle değiştir:

```cpp
static constexpr uint8_t TRANSMITTER_MAC[6] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66
};
```

Dosyayı kaydet. Alıcı ve vericide kanalın aynı, yani `6`, olduğundan emin ol.

## Aşama 7 — Kart A'yı son MAC ayarıyla yeniden yükle

1. Kart B'yi çıkar.
2. Kart A'yı tekrar bilgisayara bağla.
3. Çalıştır:

```powershell
cd "C:\Users\PC\Documents\Codex\2026-08-10\2\outputs\espnow-ruview-motion-detector\firmware\receiver"
pio run -t upload
```

Artık iki firmware de doğru MAC adreslerini içerir.

## Aşama 8 — ESP-NOW bağlantısını doğrula

1. Kart A'yı bilgisayara bağlı bırak.
2. Kart B'yi başka bir USB girişine veya USB adaptörüne bağlayarak çalıştır.
3. Önce duvar olmadan, kartları yaklaşık 2–3 metre aralıkla dene.
4. Kart A için seri monitörü aç:

```powershell
pio device monitor -b 921600
```

5. Başlangıçta şunlara benzer satırlar beklenir:

```text
Receiver STA MAC: ...
Keep the monitored area empty: 30-second calibration started.
Channel=6 model=untrained commands: LABEL, STOP, CALIBRATE, STATUS
```

6. Daha sonra `"type":"feature"` içeren JSON satırları gelmelidir.
7. Monitörde `STATUS` yazıp Enter'a bas. Çıktıda `online=yes` görülmelidir.

`online=no` görünürse önce MAC adreslerini, iki dosyadaki kanal numarasını ve Kart B'nin güç aldığını kontrol et.

## Aşama 9 — Frontend'i başlat

Frontend'e bağlanmadan önce PlatformIO seri monitörünü `Ctrl+C` ile kapat. Aynı COM portunu aynı anda yalnızca bir program kullanabilir.

Node.js 22 veya daha yeni sürüm kurulduktan sonra normal PowerShell'de çalıştır:

```powershell
cd "C:\Users\PC\Documents\Codex\2026-08-10\2\outputs\espnow-ruview-motion-detector\frontend"
corepack enable
pnpm install
pnpm run dev
```

Sonra masaüstü Chrome veya Edge ile aç:

```text
http://localhost:3000
```

1. Önce **Run demo** düğmesiyle arayüzü sensör olmadan kontrol edebilirsin.
2. Demo'yu durdur.
3. **Connect ESP32** düğmesine bas.
4. Listeden `A - RX` kartının COM portunu seç.
5. Tarayıcının seri port iznini onayla.

Gerçek bağlantıda şu alanlar çalışmalıdır:

- RSSI
- Subcarrier sayısı
- Packet sequence
- CSI activity grafiği
- Sekiz özellik değeri
- Kalibrasyon ve durum komutları

## Aşama 10 — Şu anda neden hareket sınıfı çalışmayacak?

Projede teslim edilen model dosyası güvenli bir yer tutucudur:

```text
model=untrained
SHA=UNTRAINED
```

Bu nedenle frontend'de **MODEL PENDING** ve çoğunlukla `UNKNOWN` görmek normaldir. Bu bir bağlantı hatası değildir. `EMPTY`, `STILL_PERSON` ve `MOVING_PERSON` sonuçları için kendi odanda veri toplaman gerekir.

## Aşama 11 — Eğitim verisi topla

Gerçek eğitim kaydı sırasında frontend'i ve tüm seri monitörleri kapat. Python toplayıcı COM portunu kullanacaktır.

```powershell
cd "C:\Users\PC\Documents\Codex\2026-08-10\2\outputs\espnow-ruview-motion-detector\training"
python -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
```

`COM7` yerine Kart A'nın gerçek portunu yaz. Her komut varsayılan olarak iki dakika kayıt yapar:

```powershell
python collect_serial.py --port COM7 --label empty --session empty_01
python collect_serial.py --port COM7 --label still --session still_01
python collect_serial.py --port COM7 --label moving --session moving_01
```

Toplam öneri:

- 8 farklı `empty` oturumu
- 8 farklı `still` oturumu
- 8 farklı `moving` oturumu

Her oturumda kartların yeri sabit kalmalı; insanın konumu, yönü ve hareket hızı değişmelidir.

## Aşama 12 — Modeli eğit ve ESP32 başlığına aktar

Training klasöründe çalıştır:

```powershell
python train.py --data-dir data/raw --output artifacts/ruview_motion.pt
python export_c.py --checkpoint artifacts/ruview_motion.pt --output ..\firmware\receiver\include\model_data.h
```

Eğitim komutu RuView `csi-embed-v2.safetensors` ağırlıklarını indirir ve üç sınıflı yeni başlığı senin verinle eğitir. İnternet bağlantısı gerekir.

Export başarılı olduktan sonra Kart A'yı tekrar yükle:

```powershell
cd ..\firmware\receiver
pio run -t upload
```

Frontend'e yeniden bağlandığında model göstergesi **READY** olmalıdır.

## Aşama 13 — Duvar arkasında son kurulum

1. İlk çalışmayı duvar olmadan doğrula.
2. Sonra kartları sabit ve yaklaşık aynı yükseklikte yerleştir.
3. Başlangıç için bir kartı duvarın bir tarafına, diğer kartı izlenecek alanın diğer tarafına koy; radyo bağlantı hattı izlenen alandan geçsin.
4. Kartları oynatma.
5. Alan tamamen boşken frontend'den **Calibrate 30s** düğmesine bas.
6. Kalibrasyon bitince boş, hareketsiz insan ve hareketli insan durumlarını sırayla dene.

Kartlardan biri taşınırsa veya Wi-Fi kanalı değiştirilirse yeniden kalibrasyon ve tercihen yeni eğitim verisi gerekir.

## Sık görülen sorunlar

### COM portu görünmüyor

- USB kablosunun yalnızca şarj kablosu olmadığını kontrol et.
- Farklı USB portu veya kablo dene.
- Windows Aygıt Yöneticisi'ni kontrol et.
- Gerekirse kart üreticisinin USB sürücüsünü kur.

### Firmware yüklenmiyor

- `BOOT` düğmesini basılı tut.
- `RESET/EN` düğmesine bir kez basıp bırak.
- `BOOT` düğmesini bırak ve tekrar upload dene.
- Yanlış kart modeli seçilmiş olabilir.

### Frontend seri portu açamıyor

- PlatformIO/Arduino seri monitörünü kapat.
- Chrome veya Edge kullan.
- Doğru alıcı COM portunu seç.
- Sayfayı yenileyip tekrar izin ver.

### `sensor_offline` görülüyor

- Kart B'nin açık olduğunu kontrol et.
- İki MAC dizisini tekrar kontrol et.
- İki kartta da kanalın `6` olduğunu doğrula.
- Önce kartları birbirine yaklaştırarak duvarsız dene.

### `MODEL PENDING` veya `unknown` görülüyor

Bu, henüz model eğitilip `model_data.h` içine aktarılmadığı için beklenen durumdur. CSI bağlantısı yine de çalışıyor olabilir.

## İlerleme kontrol noktaları

Kurulumu tek tek yapmak için her noktadan sonra sonucu kontrol et:

1. Kart üzerindeki tam model yazısı veya net fotoğraf
2. Kart A'nın `Receiver STA MAC` satırı
3. Kart B'nin `Transmitter STA MAC` satırı
4. Kart A'da `STATUS ... online=yes` çıktısı
5. Frontend'de değişen RSSI, packet sequence ve CSI grafiği
6. 24 kayıt dosyası
7. Eğitim metrikleri ve `MODEL READY`

Bu sistem bir prototiptir; güvenlik, sağlık veya alarm sistemi olarak tek başına kullanılmamalıdır.
