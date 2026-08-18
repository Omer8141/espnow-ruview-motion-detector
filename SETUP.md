# Kurulum — yeni bir bilgisayarda sıfırdan çalıştırma

Bu depoyu ikinci bir makineye klonladıktan sonra izlenecek adımlar. Aşağıdaki
tuzakların her biri gerçek bir hata ayıklama turuna mal oldu; sırayla gidin.

```powershell
git clone <depo-url> espnow-ruview-motion-detector
cd espnow-ruview-motion-detector
```

## 1. Firmware — PlatformIO

VS Code'un PlatformIO IDE eklentisini kurun ya da `pip install platformio`.

**`pio` PATH'e otomatik eklenmez.** Eklenti onu yalnızca kendi "PlatformIO Core CLI"
terminaline enjekte eder; normal PowerShell'de `pio ... is not recognized` hatası alırsınız.
Oturum başına:

```powershell
$env:PATH = "$env:USERPROFILE\.platformio\penv\Scripts;$env:PATH"
```

Kalıcı yapmak için (bir kez, sonra terminali yeniden açın):

```powershell
[Environment]::SetEnvironmentVariable("PATH", [Environment]::GetEnvironmentVariable("PATH","User") + ";$env:USERPROFILE\.platformio\penv\Scripts", "User")
```

İlk `pio run` espressif32 platformunu ve xtensa araç zincirini indirir (~1 GB, uzun sürer).

## 2. Kartları tanıma

**COM numaraları makineden makineye değişir.** Karta port numarasıyla değil, USB seri
numarasıyla karar verin — ESP32-S3'te bu değer STA MAC adresine eşittir:

```powershell
pio device list
```

| Kart | STA MAC (= USB seri no) | Baud |
|---|---|---|
| Alıcı (receiver) | `94:A9:90:D1:DB:40` | 921600 |
| Verici (transmitter) | `A4:CB:8F:D4:78:0C` | 115200 |

MAC'ler `firmware/*/include/peer_config.h` içinde zaten kayıtlı ve depoya dahil.
**Aynı kartları taşıdığınız sürece bunlara dokunmayın.** Farklı kartlar kullanacaksanız
her kartı bir kez flaşlayıp açılışta yazdırdığı MAC'i karşı tarafın `peer_config.h`
dosyasına yazın.

İki kart takılıyken otomatik port algılama belirsizleşir, portu açıkça verin:

```powershell
cd firmware\transmitter
pio run -t upload --upload-port COM5

cd ..\receiver
pio run -t upload --upload-port COM6
pio device monitor -b 921600
```

## 3. Python — eğitim tarafı

```powershell
pip install -r training/requirements.txt
```

Sanal ortam zorunlu değil. Doğrulama (6 test geçmeli):

```powershell
python -m pytest
```

## 4. Frontend — FieldView panosu

Node.js 22 veya üstü gerekir:

```powershell
winget install OpenJS.NodeJS
```

Kurulumdan sonra **PowerShell'i kapatıp yeniden açın**, yoksa `node` PATH'te görünmez.

**`corepack enable` çalışmaz.** Corepack, Node v25'te dağıtımdan çıkarıldı; eski
belgelerdeki bu adım güncel Node'da `corepack is not recognized` verir. Yerine:

```powershell
npm install -g pnpm
cd frontend
pnpm install
pnpm run dev
```

`http://localhost:3000/` adresini **masaüstü Chrome veya Edge** ile açın — Web Serial
Firefox ve Safari'de yok. **Connect ESP32** ile alıcının COM portunu seçin. PlatformIO
Serial Monitor açıksa önce kapatın; bir COM portunu aynı anda tek program tutabilir.

Node 26.7.0 + pnpm 11.22.0 ile doğrulandı.

## 5. Eğitim verisi git ile gelmez

`.gitignore`, `training/data/` ve `training/artifacts/` dizinlerini hariç tutar.
Toplanan CSI verisi ve eğitilmiş model dosyaları depoda taşınmaz.

Pratikte: veriyi hangi bilgisayarda topladıysanız eğitimi de orada yapın. Gerçekten
taşımanız gerekiyorsa `training/data/` klasörünü USB veya bulut ile elle kopyalayın.

Veri toplama iki yoldan yapılabilir:

- **Panodan:** sınıf ve oturum adını seçip **Start capture** → 120 sn → **Stop & save**;
  tarayıcı `train_<sinif>_<oturum>.jsonl` indirir, dosyayı `training/data/raw/` içine taşıyın.
- **Komut satırından:** `python collect_serial.py --port COM6 --label empty --session empty_01`

Her sınıf için sekiz oturum toplandıktan sonra:

```powershell
cd training
python train.py --data-dir data/raw --output artifacts/ruview_motion.pt
```

## 6. Kurulum sonrası hatırlatmalar

- Kartları duvarın iki yanına, izlenen alan **aralarında** kalacak şekilde yerleştirin.
  Yan yana dururlarken RSSI −29 dBm gibi çıkar ve duvar arkası ölçüm anlamsızlaşır.
- Kartları yerleştirdikten sonra alanı boşaltıp **Calibrate 30s** çalıştırın; ambiyans
  eşiği konuma özeldir ve NVS'te saklanır.
- Alıcının `STATUS` çıktısındaki `csi_cb` / `csi_mac_rejects` sayaçları geçici teşhis
  amaçlıdır. `csi_cb` artıyor ama `csi_mac_rejects` ona eşitse, verici kartın
  CSI üretmediği anlamına gelir (bkz. OFDM hız zorlaması, `firmware/transmitter/src/main.cpp`).
- Model henüz eğitilmemişken alıcı `unknown` bildirir ve pano **PENDING** gösterir;
  bu doğru davranıştır, sıfır ağırlıkları eğitilmiş gibi sunmaz.
