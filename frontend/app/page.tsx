"use client";

import {
  useCallback,
  useEffect,
  useRef,
  useState,
  useSyncExternalStore,
} from "react";

type DetectorState =
  | "calibrating"
  | "empty"
  | "still_person"
  | "moving_person"
  | "unknown"
  | "sensor_offline";

type LinkMode = "disconnected" | "serial" | "demo";

type FeatureFrame = {
  type: "feature";
  timestamp_us: number;
  sequence: number;
  rssi: number;
  subcarriers: number;
  features: number[];
  label?: string;
  session_id?: string;
};

type StateFrame = {
  type: "state";
  state: DetectorState;
  confidence: number;
  probabilities: number[];
  inference_us: number;
  model_sha256: string;
};

type RawCsiFrame = {
  type: "raw_csi";
  timestamp_us: number;
  sequence: number;
  source_mac: string;
  channel: number;
  rssi: number;
  csi_length: number;
  first_word_invalid: boolean;
  iq: number[];
  label?: string;
  session_id?: string;
};

type Capture = {
  label: string;
  session: string;
  lines: string[];
  rejected: number;
  startedAt: number;
};

type HistoryPoint = {
  presence: number;
  motion: number;
  rssi: number;
};

type SerialPortLike = {
  open(options: { baudRate: number }): Promise<void>;
  close(): Promise<void>;
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
};

type SerialApi = {
  requestPort(): Promise<SerialPortLike>;
};

const FEATURE_META = [
  { label: "Presence", hint: "occupancy score" },
  { label: "Motion", hint: "phase energy" },
  { label: "Breathing", hint: "slow rhythm" },
  { label: "Heart", hint: "fast rhythm" },
  { label: "Phase variance", hint: "selected carriers" },
  { label: "Person count", hint: "normalized estimate" },
  { label: "Fall flag", hint: "event feature" },
  { label: "RSSI", hint: "normalized link" },
] as const;

const STATE_COPY: Record<DetectorState, { title: string; note: string }> = {
  calibrating: {
    title: "Calibrating",
    note: "Learning the empty-room radio baseline",
  },
  empty: { title: "Room empty", note: "No person detected in the link path" },
  still_person: {
    title: "Still person",
    note: "Presence detected with low motion energy",
  },
  moving_person: {
    title: "Movement",
    note: "A moving person is affecting the CSI field",
  },
  unknown: {
    title: "Low confidence",
    note: "The model cannot assign a reliable class",
  },
  sensor_offline: {
    title: "Sensor offline",
    note: "Connect the receiver or start the demo",
  },
};

const INITIAL_FEATURES = [0.04, 0.03, 0.02, 0.01, 0.04, 0, 0, 0.62];

// The firmware rejects anything outside this set in LABEL, and train.py derives
// the session identity from the frames themselves.
const SAFE_TOKEN = /^[A-Za-z0-9_-]+$/;

// Two minutes at the receiver's 20 Hz raw output, matching collect_serial.py.
const TARGET_CAPTURE_SECONDS = 120;

const RAW_REQUIRED_KEYS = [
  "timestamp_us",
  "sequence",
  "source_mac",
  "channel",
  "rssi",
  "csi_length",
  "first_word_invalid",
  "iq",
  "label",
  "session_id",
] as const;

function clamp(value: number, min = 0, max = 1) {
  return Math.min(max, Math.max(min, value));
}

/**
 * Mirrors valid_raw_record() in training/collect_serial.py so a session captured
 * here is accepted by feature_pipeline.CsiFrame.from_json without changes.
 */
function isValidRawRecord(
  value: Record<string, unknown>,
  label: string,
  session: string,
): boolean {
  if (value.type !== "raw_csi") return false;
  for (const key of RAW_REQUIRED_KEYS) {
    if (!(key in value)) return false;
  }
  if (value.label !== label || value.session_id !== session) return false;
  const iq = value.iq;
  if (!Array.isArray(iq)) return false;
  if (iq.length !== Number(value.csi_length)) return false;
  if (iq.length % 2 !== 0 || iq.length < 4 || iq.length > 384) return false;
  return iq.every(
    (item) => Number.isInteger(item) && item >= -128 && item <= 127,
  );
}

function getSerialApi(): SerialApi | null {
  if (typeof navigator === "undefined" || !("serial" in navigator)) return null;
  return (navigator as Navigator & { serial: SerialApi }).serial;
}

function subscribeToBrowserCapabilities() {
  return () => undefined;
}

function formatState(state: DetectorState) {
  return state.replaceAll("_", " ");
}

function SignalChart({ history }: { history: HistoryPoint[] }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    const draw = () => {
      const width = canvas.clientWidth;
      const height = canvas.clientHeight;
      const ratio = window.devicePixelRatio || 1;
      canvas.width = Math.max(1, Math.floor(width * ratio));
      canvas.height = Math.max(1, Math.floor(height * ratio));
      const ctx = canvas.getContext("2d");
      if (!ctx) return;
      ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
      ctx.clearRect(0, 0, width, height);

      ctx.strokeStyle = "rgba(255,255,255,.07)";
      ctx.lineWidth = 1;
      for (let y = 12; y < height; y += 28) {
        ctx.beginPath();
        ctx.moveTo(0, y + 0.5);
        ctx.lineTo(width, y + 0.5);
        ctx.stroke();
      }

      const drawLine = (
        accessor: (point: HistoryPoint) => number,
        color: string,
        glow: boolean,
      ) => {
        if (history.length < 2) return;
        ctx.beginPath();
        history.forEach((point, index) => {
          const x = (index / Math.max(1, history.length - 1)) * width;
          const y = height - clamp(accessor(point)) * (height - 18) - 9;
          if (index === 0) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        });
        ctx.strokeStyle = color;
        ctx.lineWidth = glow ? 2.2 : 1.4;
        if (glow) {
          ctx.shadowColor = color;
          ctx.shadowBlur = 10;
        }
        ctx.stroke();
        ctx.shadowBlur = 0;
      };

      drawLine((point) => point.presence, "#b9f227", false);
      drawLine((point) => point.motion, "#ff6b44", true);
      drawLine((point) => (point.rssi + 100) / 70, "#61d8ff", false);
    };

    draw();
    const observer = new ResizeObserver(draw);
    observer.observe(canvas);
    return () => observer.disconnect();
  }, [history]);

  return (
    <canvas
      ref={canvasRef}
      className="signal-canvas"
      aria-label="Live presence, motion, and RSSI signal history"
    />
  );
}

function RoomVisualizer({
  state,
  confidence,
  rssi,
}: {
  state: DetectorState;
  confidence: number;
  rssi: number;
}) {
  return (
    <div className={`room-visual state-${state}`}>
      <div className="scan-grid" />
      <div className="noise-field" />
      <div className="sensor-node transmitter">
        <span className="node-light" />
        <strong>TX</strong>
        <small>ESP-NOW</small>
      </div>
      <div className="wall-plane">
        <span>WALL</span>
      </div>
      <div className="radio-waves" aria-hidden="true">
        <i className="wave wave-1" />
        <i className="wave wave-2" />
        <i className="wave wave-3" />
        <i className="wave wave-4" />
      </div>
      <div className="human-field" aria-hidden="true">
        <span className="human-aura" />
        <span className="human-head" />
        <span className="human-body" />
        <span className="human-arm human-arm-left" />
        <span className="human-arm human-arm-right" />
        <span className="human-leg human-leg-left" />
        <span className="human-leg human-leg-right" />
      </div>
      <div className="sensor-node receiver">
        <span className="node-light" />
        <strong>RX</strong>
        <small>CSI</small>
      </div>
      <div className="room-caption">
        <span>radio-field state view</span>
        <span>{Math.round(confidence * 100)}% confidence</span>
        <span>{rssi} dBm</span>
      </div>
    </div>
  );
}

export default function Home() {
  const [mode, setMode] = useState<LinkMode>("disconnected");
  const [state, setState] = useState<DetectorState>("sensor_offline");
  const [confidence, setConfidence] = useState(0);
  const [probabilities, setProbabilities] = useState([0, 0, 0]);
  const [features, setFeatures] = useState<number[]>(INITIAL_FEATURES);
  const [rssi, setRssi] = useState(-100);
  const [subcarriers, setSubcarriers] = useState(0);
  const [sequence, setSequence] = useState(0);
  const [inferenceUs, setInferenceUs] = useState(0);
  const [modelSha, setModelSha] = useState("UNTRAINED");
  const [history, setHistory] = useState<HistoryPoint[]>([]);
  const [events, setEvents] = useState<string[]>([
    "Dashboard ready — connect the receiver or run demo mode.",
  ]);
  const [busy, setBusy] = useState(false);
  const [sessionLabel, setSessionLabel] = useState("moving");
  const [sessionId, setSessionId] = useState("moving_01");
  const [recording, setRecording] = useState(false);
  const [recordedCount, setRecordedCount] = useState(0);
  const [recordedRejected, setRecordedRejected] = useState(0);
  const [elapsedSeconds, setElapsedSeconds] = useState(0);
  const [savedCapture, setSavedCapture] = useState<{
    label: string;
    session: string;
    frames: number;
  } | null>(null);

  const captureRef = useRef<Capture | null>(null);
  const lastCaptureRef = useRef<Capture | null>(null);
  const portRef = useRef<SerialPortLike | null>(null);
  const readerRef = useRef<ReadableStreamDefaultReader<Uint8Array> | null>(null);
  const demoTickRef = useRef(0);
  const lastFeatureRef = useRef(INITIAL_FEATURES);
  const serialSupported = useSyncExternalStore(
    subscribeToBrowserCapabilities,
    () => Boolean(getSerialApi()),
    () => false,
  );

  const addEvent = useCallback((message: string) => {
    const stamp = new Date().toLocaleTimeString([], {
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
    });
    setEvents((current) => [`${stamp}  ${message}`, ...current].slice(0, 6));
  }, []);

  // A board reset or unplug drops the link mid-capture. Keep whatever was
  // recorded so it can still be downloaded instead of silently losing it.
  const finishInterruptedCapture = useCallback(() => {
    const capture = captureRef.current;
    if (!capture) return;
    captureRef.current = null;
    setRecording(false);
    if (capture.lines.length > 0) {
      lastCaptureRef.current = capture;
      setSavedCapture({
        label: capture.label,
        session: capture.session,
        frames: capture.lines.length,
      });
      addEvent(
        `Link lost after ${capture.lines.length} frames — use the download button to keep them.`,
      );
    } else {
      addEvent("Link lost before any frames were captured.");
    }
  }, [addEvent]);

  const appendHistory = useCallback(
    (nextFeatures: number[], nextRssi: number) => {
      setHistory((current) =>
        [
          ...current,
          {
            presence: clamp(nextFeatures[0] ?? 0),
            motion: clamp(nextFeatures[1] ?? 0),
            rssi: nextRssi,
          },
        ].slice(-120),
      );
    },
    [],
  );

  const processLine = useCallback(
    (line: string) => {
      const trimmed = line.trim();
      if (!trimmed) return;

      if (!trimmed.startsWith("{")) {
        if (
          trimmed.startsWith("STATUS") ||
          trimmed.startsWith("CALIBRATION") ||
          trimmed.startsWith("LOGGING")
        ) {
          addEvent(trimmed);
        }
        return;
      }

      try {
        const frame = JSON.parse(trimmed) as
          | FeatureFrame
          | StateFrame
          | RawCsiFrame;

        if (frame.type === "feature" && frame.features?.length === 8) {
          const nextFeatures = frame.features.map((value) =>
            Number.isFinite(value) ? value : 0,
          );
          lastFeatureRef.current = nextFeatures;
          setFeatures(nextFeatures);
          setRssi(frame.rssi);
          setSubcarriers(frame.subcarriers);
          setSequence(frame.sequence);
          appendHistory(nextFeatures, frame.rssi);
        }

        if (frame.type === "state") {
          setState(frame.state);
          setConfidence(clamp(frame.confidence));
          // Must wrap: passing clamp directly to map feeds the element index as
          // `min` and the whole array as `max`, which yields NaN.
          setProbabilities(
            frame.probabilities.slice(0, 3).map((value) => clamp(value)),
          );
          setInferenceUs(frame.inference_us);
          setModelSha(frame.model_sha256 || "UNKNOWN");
          addEvent(`State changed to ${formatState(frame.state)}.`);
        }

        if (frame.type === "raw_csi" && Array.isArray(frame.iq)) {
          const capture = captureRef.current;
          if (capture) {
            if (
              isValidRawRecord(
                frame as unknown as Record<string, unknown>,
                capture.label,
                capture.session,
              )
            ) {
              // Stored verbatim: the firmware already emits compact JSON that
              // feature_pipeline.py parses directly.
              capture.lines.push(trimmed);
              setRecordedCount(capture.lines.length);
            } else {
              capture.rejected += 1;
              setRecordedRejected(capture.rejected);
            }
          }

          let total = 0;
          for (let index = 0; index + 1 < frame.iq.length; index += 2) {
            total += Math.hypot(frame.iq[index], frame.iq[index + 1]);
          }
          const carriers = Math.max(1, Math.floor(frame.iq.length / 2));
          const rawEnergy = clamp(total / carriers / 128);
          const rawFeatures = [...lastFeatureRef.current];
          rawFeatures[1] = rawEnergy;
          setRssi(frame.rssi);
          setSubcarriers(carriers);
          setSequence(frame.sequence);
          appendHistory(rawFeatures, frame.rssi);
        }
      } catch {
        // Boot messages and partially received lines are intentionally ignored.
      }
    },
    [addEvent, appendHistory],
  );

  const disconnectSerial = useCallback(async () => {
    const activeReader = readerRef.current;
    readerRef.current = null;
    if (activeReader) {
      try {
        await activeReader.cancel();
      } catch {
        // Port may already be gone after a board reset.
      }
    }
    const activePort = portRef.current;
    portRef.current = null;
    if (activePort) {
      try {
        await activePort.close();
      } catch {
        // Reader cleanup will release a still-locked port.
      }
    }
    finishInterruptedCapture();
    setMode("disconnected");
    setState("sensor_offline");
    setConfidence(0);
  }, [finishInterruptedCapture]);

  const readSerial = useCallback(
    async (port: SerialPortLike) => {
      if (!port.readable) return;
      const reader = port.readable.getReader();
      readerRef.current = reader;
      const decoder = new TextDecoder();
      let buffer = "";

      try {
        while (true) {
          const { value, done } = await reader.read();
          if (done) break;
          buffer += decoder.decode(value, { stream: true });
          const lines = buffer.split(/\r?\n/);
          buffer = lines.pop() ?? "";
          lines.forEach(processLine);
        }
      } catch (error) {
        if (portRef.current === port) {
          addEvent(
            `Serial link stopped: ${error instanceof Error ? error.message : "unknown error"}`,
          );
        }
      } finally {
        try {
          reader.releaseLock();
        } catch {
          // Lock can already be released by the browser.
        }
        if (readerRef.current === reader) readerRef.current = null;
        if (portRef.current === port) {
          portRef.current = null;
          try {
            await port.close();
          } catch {
            // The operating system may have removed the USB port.
          }
          finishInterruptedCapture();
          setMode("disconnected");
          setState("sensor_offline");
          setConfidence(0);
        }
      }
    },
    [addEvent, finishInterruptedCapture, processLine],
  );

  const connectSerial = useCallback(async () => {
    const serial = getSerialApi();
    if (!serial) {
      addEvent("Web Serial needs desktop Chrome or Edge.");
      return;
    }

    setBusy(true);
    try {
      const port = await serial.requestPort();
      await port.open({ baudRate: 921600 });
      portRef.current = port;
      setMode("serial");
      setState("unknown");
      setHistory([]);
      addEvent("Receiver connected at 921600 baud.");
      void readSerial(port);
    } catch (error) {
      addEvent(
        `Connection cancelled: ${error instanceof Error ? error.message : "unknown error"}`,
      );
    } finally {
      setBusy(false);
    }
  }, [addEvent, readSerial]);

  const sendCommand = useCallback(
    async (command: string) => {
      const port = portRef.current;
      if (!port?.writable) {
        addEvent("Connect the receiver before sending commands.");
        return;
      }
      const writer = port.writable.getWriter();
      try {
        await writer.write(new TextEncoder().encode(`${command}\n`));
        addEvent(`Sent: ${command}`);
      } finally {
        writer.releaseLock();
      }
    },
    [addEvent],
  );

  const downloadCapture = useCallback(
    (capture: Capture) => {
      const blob = new Blob([`${capture.lines.join("\n")}\n`], {
        type: "application/x-ndjson",
      });
      const url = URL.createObjectURL(blob);
      const anchor = document.createElement("a");
      anchor.href = url;
      anchor.download = `train_${capture.label}_${capture.session}.jsonl`;
      document.body.append(anchor);
      anchor.click();
      anchor.remove();
      URL.revokeObjectURL(url);
      addEvent(
        `Saved ${capture.lines.length} frames to train_${capture.label}_${capture.session}.jsonl`,
      );
    },
    [addEvent],
  );

  const startCapture = useCallback(async () => {
    const session = sessionId.trim();
    if (!SAFE_TOKEN.test(session)) {
      addEvent("Session id must use only letters, numbers, '_' or '-'.");
      return;
    }
    if (!portRef.current?.writable) {
      addEvent("Connect the receiver before capturing.");
      return;
    }
    captureRef.current = {
      label: sessionLabel,
      session,
      lines: [],
      rejected: 0,
      startedAt: Date.now(),
    };
    setRecordedCount(0);
    setRecordedRejected(0);
    setElapsedSeconds(0);
    setSavedCapture(null);
    setRecording(true);
    await sendCommand(`LABEL ${sessionLabel} ${session}`);
  }, [addEvent, sendCommand, sessionId, sessionLabel]);

  const stopCapture = useCallback(async () => {
    await sendCommand("STOP");
    const capture = captureRef.current;
    captureRef.current = null;
    setRecording(false);
    if (!capture) return;
    if (capture.lines.length === 0) {
      addEvent("No valid CSI frames captured — nothing saved.");
      return;
    }
    lastCaptureRef.current = capture;
    setSavedCapture({
      label: capture.label,
      session: capture.session,
      frames: capture.lines.length,
    });
    downloadCapture(capture);
  }, [addEvent, downloadCapture, sendCommand]);

  useEffect(() => {
    if (!recording) return;
    const timer = window.setInterval(() => {
      const capture = captureRef.current;
      if (capture) {
        setElapsedSeconds(Math.floor((Date.now() - capture.startedAt) / 1000));
      }
    }, 500);
    return () => window.clearInterval(timer);
  }, [recording]);

  useEffect(() => {
    return () => {
      void disconnectSerial();
    };
  }, [disconnectSerial]);

  useEffect(() => {
    if (mode !== "demo") return;
    const timer = window.setInterval(() => {
      const tick = demoTickRef.current++;
      const phase = tick % 80;
      let nextState: DetectorState = "empty";
      if (phase >= 18 && phase < 42) nextState = "moving_person";
      else if (phase >= 42 && phase < 62) nextState = "still_person";
      else if (phase >= 62 && phase < 72) nextState = "moving_person";

      const moving = nextState === "moving_person";
      const still = nextState === "still_person";
      const noise = Math.sin(tick * 0.71) * 0.025;
      const nextFeatures = [
        clamp(moving ? 0.91 + noise : still ? 0.84 + noise : 0.07 + noise),
        clamp(moving ? 0.79 + Math.sin(tick * 1.6) * 0.13 : still ? 0.12 : 0.045),
        clamp(still ? 0.58 + Math.sin(tick * 0.28) * 0.09 : 0.13),
        clamp(still ? 0.37 + Math.sin(tick * 0.62) * 0.08 : 0.09),
        clamp(moving ? 0.73 + noise : still ? 0.31 : 0.08),
        moving || still ? 0.5 : 0,
        0,
        clamp(0.66 + Math.sin(tick * 0.12) * 0.04),
      ];
      const nextRssi = Math.round(-53 + Math.sin(tick * 0.17) * 4);
      const nextConfidence = moving ? 0.92 : still ? 0.86 : 0.96;
      const nextProbabilities = moving
        ? [0.03, 0.05, 0.92]
        : still
          ? [0.06, 0.86, 0.08]
          : [0.96, 0.02, 0.02];

      if (nextState !== state && tick > 0) {
        addEvent(`Demo state changed to ${formatState(nextState)}.`);
      }
      lastFeatureRef.current = nextFeatures;
      setState(nextState);
      setConfidence(nextConfidence);
      setProbabilities(nextProbabilities);
      setFeatures(nextFeatures);
      setRssi(nextRssi);
      setSubcarriers(64);
      setSequence((current) => current + 5);
      setInferenceUs(7340 + Math.round(Math.sin(tick) * 410));
      setModelSha("DEMO-csi-embed-v2");
      appendHistory(nextFeatures, nextRssi);
    }, 250);
    return () => window.clearInterval(timer);
  }, [addEvent, appendHistory, mode, state]);

  const toggleDemo = async () => {
    if (mode === "demo") {
      setMode("disconnected");
      setState("sensor_offline");
      setConfidence(0);
      addEvent("Demo stopped.");
      return;
    }
    if (mode === "serial") await disconnectSerial();
    demoTickRef.current = 0;
    setHistory([]);
    setMode("demo");
    addEvent("Demo started with simulated CSI features.");
  };

  const stateCopy = STATE_COPY[state];
  const modelReady =
    modelSha !== "UNTRAINED" && modelSha !== "UNKNOWN" && !modelSha.startsWith("0");

  return (
    <main className="app-shell">
      <header className="topbar">
        <a className="brand" href="#top" aria-label="FieldView home">
          <span className="brand-mark" aria-hidden="true">
            <i />
            <i />
            <i />
          </span>
          <span>
            <strong>FIELD/VIEW</strong>
            <small>ESP-NOW CSI OBSERVATORY</small>
          </span>
        </a>
        <div className="topbar-status">
          <span className={`live-dot mode-${mode}`} />
          <span>{mode === "serial" ? "RECEIVER LIVE" : mode === "demo" ? "DEMO SIGNAL" : "NO LINK"}</span>
          <span className="topbar-divider" />
          <span>CH 6 · 20 Hz packets · 4 Hz inference</span>
        </div>
        <div className="header-actions">
          <button className="button button-ghost" onClick={() => void toggleDemo()}>
            {mode === "demo" ? "Stop demo" : "Run demo"}
          </button>
          {mode === "serial" ? (
            <button className="button button-primary" onClick={() => void disconnectSerial()}>
              Disconnect
            </button>
          ) : (
            <button
              className="button button-primary"
              onClick={() => void connectSerial()}
              disabled={busy || !serialSupported}
              title={!serialSupported ? "Use Chrome or Edge on desktop" : undefined}
            >
              {busy ? "Connecting…" : "Connect ESP32"}
            </button>
          )}
        </div>
      </header>

      <section className="intro" id="top">
        <div>
          <p className="eyebrow">THROUGH-WALL MOTION / LOCAL INFERENCE</p>
          <h1>See the radio field react.</h1>
        </div>
        <p className="intro-copy">
          Live ESP32-S3 CSI features, RuView-initialized class probabilities, and
          room state — directly from USB. No access point and no cloud stream.
        </p>
      </section>

      {!serialSupported && (
        <aside className="compatibility-note">
          Web Serial is unavailable in this browser. Open this page in desktop
          Chrome or Edge, or use demo mode to explore the dashboard.
        </aside>
      )}

      <section className="dashboard-grid">
        <article className="panel spatial-panel">
          <div className="panel-heading">
            <div>
              <p className="panel-kicker">LIVE SPATIAL INFERENCE</p>
              <h2>Link path</h2>
            </div>
            <span className={`state-tag state-tag-${state}`}>{formatState(state)}</span>
          </div>
          <RoomVisualizer state={state} confidence={confidence} rssi={rssi} />
          <p className="truth-note">
            State visualization — not a reconstructed body pose. True joint
            tracking requires a separately trained keypoint model.
          </p>
        </article>

        <article className={`panel verdict-panel verdict-${state}`}>
          <div className="panel-heading compact">
            <p className="panel-kicker">DETECTOR VERDICT</p>
            <span className="state-index">01</span>
          </div>
          <div className="verdict-icon" aria-hidden="true">
            <span />
          </div>
          <p className="verdict-title">{stateCopy.title}</p>
          <p className="verdict-note">{stateCopy.note}</p>
          <div className="confidence-readout">
            <span>CONFIDENCE</span>
            <strong>{Math.round(confidence * 100)}%</strong>
          </div>
          <div className="confidence-track">
            <i style={{ width: `${confidence * 100}%` }} />
          </div>
          <dl className="micro-metrics">
            <div>
              <dt>Inference</dt>
              <dd>{inferenceUs ? `${(inferenceUs / 1000).toFixed(1)} ms` : "—"}</dd>
            </div>
            <div>
              <dt>Threshold</dt>
              <dd>65%</dd>
            </div>
          </dl>
        </article>

        <article className="panel signal-panel">
          <div className="panel-heading">
            <div>
              <p className="panel-kicker">ROLLING SIGNAL / 30 SEC</p>
              <h2>CSI activity</h2>
            </div>
            <div className="chart-legend">
              <span className="legend-presence">Presence</span>
              <span className="legend-motion">Motion</span>
              <span className="legend-rssi">RSSI</span>
            </div>
          </div>
          <SignalChart history={history} />
          <dl className="signal-stats">
            <div>
              <dt>RSSI</dt>
              <dd>{rssi} <small>dBm</small></dd>
            </div>
            <div>
              <dt>Subcarriers</dt>
              <dd>{subcarriers || "—"}</dd>
            </div>
            <div>
              <dt>Packet seq.</dt>
              <dd>{sequence || "—"}</dd>
            </div>
            <div>
              <dt>Model</dt>
              <dd className={modelReady ? "model-ready" : "model-pending"}>
                {modelReady ? "READY" : "PENDING"}
              </dd>
            </div>
          </dl>
        </article>

        <article className="panel probability-panel">
          <div className="panel-heading compact">
            <div>
              <p className="panel-kicker">SOFTMAX OUTPUT</p>
              <h2>Class probability</h2>
            </div>
            <span className="state-index">02</span>
          </div>
          {[
            ["Empty", probabilities[0]],
            ["Still person", probabilities[1]],
            ["Moving person", probabilities[2]],
          ].map(([label, value]) => (
            <div className="probability-row" key={label as string}>
              <div>
                <span>{label}</span>
                <strong>{Math.round((value as number) * 100)}%</strong>
              </div>
              <div className="probability-track">
                <i style={{ width: `${(value as number) * 100}%` }} />
              </div>
            </div>
          ))}
          <p className="model-sha" title={modelSha}>
            SHA / {modelSha.length > 22 ? `${modelSha.slice(0, 22)}…` : modelSha}
          </p>
        </article>

        <article className="panel features-panel">
          <div className="panel-heading">
            <div>
              <p className="panel-kicker">RUVIEW-COMPATIBLE INPUT</p>
              <h2>Eight-feature encoder frame</h2>
            </div>
            <span className="sampling-note">updated every 250 ms</span>
          </div>
          <div className="feature-grid">
            {FEATURE_META.map((meta, index) => {
              const value = clamp(Number(features[index] ?? 0));
              return (
                <div className="feature-cell" key={meta.label}>
                  <div className="feature-label">
                    <span>{String(index + 1).padStart(2, "0")}</span>
                    <div>
                      <strong>{meta.label}</strong>
                      <small>{meta.hint}</small>
                    </div>
                    <b>{value.toFixed(2)}</b>
                  </div>
                  <div className="feature-track">
                    <i style={{ width: `${value * 100}%` }} />
                  </div>
                </div>
              );
            })}
          </div>
        </article>

        <article className="panel control-panel">
          <div className="panel-heading compact">
            <div>
              <p className="panel-kicker">RECEIVER CONTROL</p>
              <h2>Commands</h2>
            </div>
            <span className="state-index">03</span>
          </div>
          <div className="command-row">
            <button onClick={() => void sendCommand("CALIBRATE")}>Calibrate 30s</button>
            <button onClick={() => void sendCommand("STATUS")}>Request status</button>
          </div>
          <div className="capture-form">
            <label>
              CLASS LABEL
              <select value={sessionLabel} onChange={(event) => setSessionLabel(event.target.value)}>
                <option value="empty">Empty</option>
                <option value="still">Still person</option>
                <option value="moving">Moving person</option>
              </select>
            </label>
            <label>
              SESSION ID
              <input
                value={sessionId}
                onChange={(event) => setSessionId(event.target.value.replace(/\s+/g, "_"))}
                placeholder="moving_01"
              />
            </label>
          </div>
          <div className="command-row">
            <button
              className="capture-button"
              onClick={() => void startCapture()}
              disabled={recording}
            >
              {recording ? "Recording…" : "Start capture"}
            </button>
            <button onClick={() => void stopCapture()} disabled={!recording}>
              Stop &amp; save
            </button>
          </div>

          <div className={`recorder-readout${recording ? " is-recording" : ""}`}>
            <div className="recorder-line">
              <span className="recorder-dot" aria-hidden="true" />
              <strong>
                {recording
                  ? `Recording ${sessionLabel} / ${sessionId}`
                  : "Recorder idle"}
              </strong>
            </div>
            <dl className="micro-metrics">
              <div>
                <dt>Frames</dt>
                <dd>{recordedCount || "—"}</dd>
              </div>
              <div>
                <dt>Elapsed</dt>
                <dd>
                  {recording || elapsedSeconds
                    ? `${elapsedSeconds}s / ${TARGET_CAPTURE_SECONDS}s`
                    : "—"}
                </dd>
              </div>
              <div>
                <dt>Rejected</dt>
                <dd>{recordedRejected || "—"}</dd>
              </div>
            </dl>
            {recording && (
              <div className="recorder-track">
                <i
                  style={{
                    width: `${clamp(elapsedSeconds / TARGET_CAPTURE_SECONDS) * 100}%`,
                  }}
                />
              </div>
            )}
            {savedCapture && !recording && (
              <button
                className="recorder-redownload"
                onClick={() => {
                  const capture = lastCaptureRef.current;
                  if (capture) downloadCapture(capture);
                }}
              >
                Download train_{savedCapture.label}_{savedCapture.session}.jsonl
                again ({savedCapture.frames} frames)
              </button>
            )}
          </div>

          <p className="control-note">
            Stopping a capture downloads a <code>.jsonl</code> file in the exact
            format <code>train.py</code> expects. Move it into{" "}
            <code>training/data/raw/</code>, collect eight sessions per class,
            then run the trainer.
          </p>
        </article>

        <article className="panel event-panel">
          <div className="panel-heading compact">
            <div>
              <p className="panel-kicker">SYSTEM EVENT LOG</p>
              <h2>Latest activity</h2>
            </div>
            <span className={`link-badge link-${mode}`}>{mode}</span>
          </div>
          <ol className="event-list">
            {events.map((event, index) => (
              <li key={`${event}-${index}`}>
                <span>{String(index + 1).padStart(2, "0")}</span>
                <p>{event}</p>
              </li>
            ))}
          </ol>
        </article>
      </section>

      <footer>
        <p>FIELD/VIEW · ESP-NOW THROUGH-WALL MOTION PROTOTYPE</p>
        <p>Privacy first — sensor data stays between your browser and receiver.</p>
      </footer>
    </main>
  );
}
