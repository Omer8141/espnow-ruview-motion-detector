from __future__ import annotations

import argparse
import json
import re
import time
from pathlib import Path

import serial


SAFE_TOKEN = re.compile(r"^[A-Za-z0-9_-]+$")


def valid_raw_record(value: dict, label: str, session: str) -> bool:
    if value.get("type") != "raw_csi":
        return False
    required = {
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
    }
    if not required.issubset(value):
        return False
    if value["label"] != label or value["session_id"] != session:
        return False
    iq = value["iq"]
    return (
        isinstance(iq, list)
        and len(iq) == int(value["csi_length"])
        and len(iq) % 2 == 0
        and 4 <= len(iq) <= 384
        and all(isinstance(item, int) and -128 <= item <= 127 for item in iq)
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Collect a labeled ESP32 CSI session")
    parser.add_argument("--port", required=True)
    parser.add_argument("--label", choices=("empty", "still", "moving"), required=True)
    parser.add_argument("--session", required=True)
    parser.add_argument("--duration", type=float, default=120.0)
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--output-dir", type=Path, default=Path("data/raw"))
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    if not SAFE_TOKEN.fullmatch(args.session):
        raise SystemExit("session must contain only letters, numbers, '_' or '-'")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    output = args.output_dir / f"train_{args.label}_{args.session}.jsonl"
    if output.exists() and not args.overwrite:
        raise SystemExit(f"refusing to overwrite {output}; pass --overwrite if intentional")

    accepted = 0
    rejected = 0
    started = time.monotonic()
    with serial.Serial(args.port, args.baud, timeout=1) as device, output.open(
        "w", encoding="utf-8", newline="\n"
    ) as handle:
        time.sleep(1.0)
        device.reset_input_buffer()
        device.write(f"LABEL {args.label} {args.session}\n".encode("ascii"))
        try:
            while time.monotonic() - started < args.duration:
                raw = device.readline()
                if not raw:
                    continue
                try:
                    value = json.loads(raw.decode("utf-8", errors="strict"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    rejected += 1
                    continue
                if valid_raw_record(value, args.label, args.session):
                    handle.write(json.dumps(value, separators=(",", ":")) + "\n")
                    accepted += 1
                elif value.get("type") == "raw_csi":
                    rejected += 1
        finally:
            device.write(b"STOP\n")

    if accepted == 0:
        output.unlink(missing_ok=True)
        raise SystemExit("no valid raw CSI frames received")
    print(f"Saved {accepted} frames to {output}; rejected {rejected}")


if __name__ == "__main__":
    main()

