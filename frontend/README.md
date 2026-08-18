# FieldView frontend

FieldView is the live browser dashboard for the ESP-NOW RuView motion detector. It reads the receiver's existing JSONL stream over USB Web Serial and shows:

- detector state and confidence;
- `EMPTY`, `STILL_PERSON`, and `MOVING_PERSON` probabilities;
- rolling presence, motion, and RSSI signals;
- all eight RuView-compatible input features;
- packet sequence, subcarrier count, inference time, and model readiness;
- calibration, status, and labeled-capture controls.

The animated person is a state visualization. It is not a measured 17-joint skeleton: the deployed network has three class outputs and cannot reconstruct body pose.

## Run locally on Windows

Install Node.js 22 or newer (`winget install OpenJS.NodeJS`), reopen PowerShell so
`node` lands on PATH, then from this folder:

```powershell
npm install -g pnpm
pnpm install
pnpm run dev
```

Older instructions used `corepack enable` to provide `pnpm`. Corepack was unbundled
from Node in v25, so on current Node it is not present and `npm install -g pnpm`
is the working equivalent.

Verified on Node 26.7.0 with pnpm 11.22.0; the dev server listens on
`http://localhost:3000/`.

Open `http://localhost:3000` in desktop Chrome or Microsoft Edge. Click **Connect ESP32**, choose the receiver's USB serial port, and approve access. The dashboard opens it at `921600` baud.

Firefox and Safari do not currently expose Web Serial. Demo mode works without hardware in any modern browser.

## Use with the receiver

1. Flash the receiver firmware and connect its USB serial port.
2. Close PlatformIO Serial Monitor, Arduino Serial Monitor, or any other program holding that COM port.
3. Start this frontend and click **Connect ESP32**.
4. If the model is still the placeholder, the dashboard correctly shows **PENDING** and the receiver will report `unknown`.
5. After exporting and flashing your trained `model_data.h`, reconnect and confirm the model indicator changes to **READY**.
6. Keep both boards fixed and use **Calibrate 30s** with the monitored area empty.

The page sends the firmware commands `CALIBRATE`, `STATUS`, `LABEL ...`, and `STOP` over the same serial link. Raw CSI appears in the rolling chart while labeled capture is active.

## Collect training sessions from the browser

The **Receiver control** panel records labeled sessions without the Python collector:

1. Pick a class label and a session id (letters, numbers, `_` and `-` only — the firmware rejects anything else).
2. Click **Start capture**. The recorder shows live frame count, elapsed time against the 120-second target, and how many frames were rejected.
3. Click **Stop & save**. The browser downloads `train_<label>_<session>.jsonl`.
4. Move the downloaded files into `training/data/raw/`.

Frames are validated in the browser with the same rules as `training/collect_serial.py`, and written verbatim as the firmware emits them, so `train.py` accepts the files unchanged. Collect eight sessions per class, then:

```powershell
cd ../training
python train.py --data-dir data/raw --output artifacts/ruview_motion.pt
```

Model training itself stays in Python — PyTorch and the RuView safetensors download cannot run in a browser. The dashboard covers labeled data collection; `train.py` and `export_c.py` do the rest.

If the link drops mid-capture, the frames recorded so far are kept and a download button appears in the panel.

## Validation

```powershell
pnpm run lint
pnpm test
```

The production build is generated with `pnpm run build`.
