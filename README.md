# ESP-NOW RuView Motion Detector

This project turns two ESP32-S3 boards into a connectionless Wi-Fi CSI detector. It does not join an access point: one board sends ESP-NOW probes and the other captures their CSI, derives RuView-compatible features, and runs a three-class model locally.

The delivered firmware is safe before training. Its placeholder model has `kRuvModelReady=false`, so it reports `UNKNOWN` rather than presenting zero-filled weights as a trained detector.

## Project contents

- `firmware/transmitter` — 20 Hz ESP-NOW unicast probe.
- `firmware/receiver` — CSI queue, feature extraction, serial recorder, calibration, int8 inference, and state debounce.
- `training` — serial collection, Python reference features, RuView transfer learning, and C-header export.
- `tests` — feature, split, BatchNorm-folding, and quantization tests.

- `frontend` — live RuView-style Web Serial dashboard with demo mode and receiver controls.

## 1. Configure the boards

Both boards must use the same channel. The default is channel 6.

1. Build and flash the receiver once. At boot it prints `Receiver STA MAC`.
2. Put that address in `firmware/transmitter/include/peer_config.h`.
3. Build and flash the transmitter. It prints `Transmitter STA MAC`.
4. Put that address in `firmware/receiver/include/peer_config.h` and flash the receiver again.

The projects target Arduino ESP32 Core through PlatformIO:

```powershell
cd firmware/transmitter
pio run -t upload

cd ../receiver
pio run -t upload
pio device monitor -b 921600
```

If a different ESP32-S3 board definition is required, change `board` in each `platformio.ini`. Keep power saving disabled and keep both devices stationary after calibration.

## 2. Calibrate the empty area

The receiver automatically performs a 30-second empty-room calibration on its first boot. The resulting ambient threshold is stored in NVS. Move neither board after calibration.

To recalibrate, send this line over the receiver serial port and leave the monitored area empty:

```text
CALIBRATE
```

Other receiver commands are:

```text
LABEL empty empty_near_01
LABEL still still_sitting_01
LABEL moving moving_walk_01
STOP
STATUS
```

## 3. Collect the 24 sessions

Create a Python environment and install the training dependencies:

```powershell
cd training
python -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
```

Collect eight independent, two-minute sessions for each label. Use unique session names:

```powershell
python collect_serial.py --port COM7 --label empty --session empty_near_01
python collect_serial.py --port COM7 --label still --session still_sitting_01
python collect_serial.py --port COM7 --label moving --session moving_walk_01
```

The collector writes validated JSONL files under `training/data/raw`. Vary position, distance, orientation, sitting/standing, and movement speed. Empty sessions should include normal disturbances such as doors, fans, and nearby wireless traffic.

## 4. Train from RuView v2

The trainer downloads `ruvnet/wifi-densepose-pretrained/csi-embed-v2.safetensors`, loads the pretrained `8 -> 64 -> 128` encoder, and trains a new `128 -> 3` head. It uses complete recording sessions for the 5/1/2 train/validation/test split.

```powershell
python train.py --data-dir data/raw --output artifacts/ruview_motion.pt
```

If the frozen encoder does not reach validation macro-F1 0.85, the trainer automatically fine-tunes the second encoder block and then the full encoder from the pretrained weights. It prints both a raw-feature logistic benchmark and held-out model metrics.

## 5. Export and flash the on-device model

Export refuses a checkpoint that misses the requested macro-F1 or moving-recall targets. It also refuses int8 conversion when held-out float/int8 class agreement is below 98%.

Run this command from the `training` directory:

```powershell
python export_c.py `
  --checkpoint artifacts/ruview_motion.pt `
  --output ../firmware/receiver/include/model_data.h
```

Then rebuild and flash the receiver:

```powershell
cd ../firmware/receiver
pio run -t upload
pio device monitor -b 921600
```

The receiver reports these states:

- `calibrating`
- `empty`
- `still_person`
- `moving_person`
- `unknown`
- `sensor_offline`

It emits JSON state events including confidence, three probabilities, measured inference time, and the exported model hash.

## 6. Open the live frontend

The included FieldView dashboard uses the receiver's existing `921600`-baud JSONL output; no additional firmware protocol is required.

```powershell
cd ../../frontend
npm install -g pnpm
pnpm install
pnpm run dev
```

Node.js 22 or newer is required (`winget install OpenJS.NodeJS`). Corepack was
unbundled from Node in v25, so `corepack enable` no longer works on current
releases; install `pnpm` through npm instead.

Open `http://localhost:3000` in desktop Chrome or Microsoft Edge and click **Connect ESP32**. Close PlatformIO/Arduino serial monitors first because only one program can own the COM port. You can use **Run demo** before connecting hardware.

The dashboard visualizes the radio field, detector state, three class probabilities, eight input features, live signal history, calibration, and data-capture controls. It does not claim to reconstruct a body skeleton: the current model produces three classes, not joint coordinates. See `frontend/README.md` for details.

## Verification

Run the desktop tests before collecting data:

```powershell
python -m pytest
```

After deployment, confirm:

- held-out macro-F1 is at least 0.85;
- moving recall is at least 0.90;
- a 30-minute empty-room run produces at most one false moving event;
- typical motion latency is no more than 1.5 seconds;
- measured ESP32 inference latency is below 20 ms;
- transmitter loss produces `sensor_offline`, never `empty`.

The repository contains no claim that these targets have already been achieved. They require your room-specific data and real-hardware measurements.

## Model and protocol notes

RuView's v2 model is a temporal CSI embedding, not a universal downstream motion classifier. This project reuses its weights and trains the missing room-specific task head. The old published presence head is not used.

ESP-NOW still uses the ESP32 Wi-Fi radio but does not require association with a router. Both peers must stay on the same configured channel. The receiver filters ESP-NOW and CSI input by the configured transmitter MAC.

References:

- [RuView pretrained model](https://huggingface.co/ruvnet/wifi-densepose-pretrained)
- [RuView v2 model scope and re-benchmark](https://github.com/ruvnet/RuView/issues/882)
- [ESP32-S3 ESP-NOW documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/network/esp_now.html)
- [Espressif ESP-CSI examples](https://github.com/espressif/esp-csi)
