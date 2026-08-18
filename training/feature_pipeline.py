from __future__ import annotations

import argparse
import json
import math
import random
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import numpy as np


MAX_SUBCARRIERS = 128
TOP_K = 8
HISTORY = 256
SAMPLE_RATE_HZ = 20.0
FEATURE_EVERY_FRAMES = 5
MOTION_WINDOW = 20
LABELS = ("empty", "still", "moving")


@dataclass(frozen=True)
class CsiFrame:
    timestamp_us: int
    sequence: int
    source_mac: str
    channel: int
    rssi: int
    first_word_invalid: bool
    iq: np.ndarray
    label: str
    session_id: str

    @classmethod
    def from_json(cls, value: dict) -> "CsiFrame":
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
        missing = required.difference(value)
        if missing:
            raise ValueError(f"missing CSI fields: {sorted(missing)}")
        if value.get("type", "raw_csi") != "raw_csi":
            raise ValueError("not a raw CSI record")
        label = str(value["label"])
        if label not in LABELS:
            raise ValueError(f"unsupported label: {label}")
        raw_iq = np.asarray(value["iq"], dtype=np.int16)
        expected = int(value["csi_length"])
        if raw_iq.ndim != 1 or raw_iq.size != expected:
            raise ValueError(f"csi_length={expected}, actual={raw_iq.size}")
        if raw_iq.size < 4 or raw_iq.size > 384 or raw_iq.size % 2:
            raise ValueError(f"invalid CSI byte length: {raw_iq.size}")
        if np.any(raw_iq < -128) or np.any(raw_iq > 127):
            raise ValueError("I/Q byte outside int8 range")
        return cls(
            timestamp_us=int(value["timestamp_us"]),
            sequence=int(value["sequence"]),
            source_mac=str(value["source_mac"]),
            channel=int(value["channel"]),
            rssi=int(value["rssi"]),
            first_word_invalid=bool(value["first_word_invalid"]),
            iq=raw_iq.astype(np.int8),
            label=label,
            session_id=str(value["session_id"]),
        )


class BandFilter:
    def __init__(self) -> None:
        self.previous_input = 0.0
        self.highpass = 0.0
        self.lowpass = 0.0
        self.initialized = False

    def process(self, value: float, low_cut_hz: float, high_cut_hz: float) -> float:
        dt = 1.0 / SAMPLE_RATE_HZ
        if not self.initialized:
            self.previous_input = value
            self.initialized = True
        high_rc = 1.0 / (2.0 * math.pi * low_cut_hz)
        high_alpha = high_rc / (high_rc + dt)
        self.highpass = high_alpha * (
            self.highpass + value - self.previous_input
        )
        self.previous_input = value
        low_rc = 1.0 / (2.0 * math.pi * high_cut_hz)
        low_alpha = dt / (low_rc + dt)
        self.lowpass += low_alpha * (self.highpass - self.lowpass)
        return self.lowpass


class FeatureExtractor:
    """Python reference for firmware/receiver/src/ruview_features.cpp."""

    def __init__(self, ambient_threshold: float = 1.0e-5) -> None:
        self.ambient_threshold = max(float(ambient_threshold), 1.0e-5)
        self.count = np.zeros(MAX_SUBCARRIERS, dtype=np.int64)
        self.mean = np.zeros(MAX_SUBCARRIERS, dtype=np.float64)
        self.m2 = np.zeros(MAX_SUBCARRIERS, dtype=np.float64)
        self.previous_phase = np.zeros(MAX_SUBCARRIERS, dtype=np.float64)
        self.phase_initialized = np.zeros(MAX_SUBCARRIERS, dtype=bool)
        self.top_k = np.empty(0, dtype=np.int64)
        self.phase_history = np.zeros(HISTORY, dtype=np.float64)
        self.breathing_history = np.zeros(HISTORY, dtype=np.float64)
        self.heart_history = np.zeros(HISTORY, dtype=np.float64)
        self.history_head = 0
        self.history_count = 0
        self.breathing_filter = BandFilter()
        self.heart_filter = BandFilter()
        self.frame_count = 0
        self.motion_energy = 0.0
        self.previous_velocity = 0.0
        self.fall_consecutive = 0
        self.fall_hold_until_us = 0

    def _variance(self) -> np.ndarray:
        output = np.zeros(MAX_SUBCARRIERS, dtype=np.float64)
        valid = self.count > 1
        output[valid] = self.m2[valid] / (self.count[valid] - 1)
        return output

    def _update_welford(self, index: int, value: float) -> None:
        self.count[index] += 1
        delta = value - self.mean[index]
        self.mean[index] += delta / self.count[index]
        delta2 = value - self.mean[index]
        self.m2[index] += delta * delta2

    @staticmethod
    def _unwrap(previous: float, current: float) -> float:
        difference = current - previous
        if difference > math.pi:
            difference -= 2.0 * math.pi
        if difference < -math.pi:
            difference += 2.0 * math.pi
        return previous + difference

    def _update_top_k(self, subcarrier_count: int) -> None:
        values = self._variance()[:subcarrier_count]
        self.top_k = np.argsort(-values, kind="stable")[: min(TOP_K, subcarrier_count)]

    def _recent_variance(self, window: int) -> float:
        count = min(window, self.history_count)
        if count < 2:
            return 0.0
        indices = [
            (self.history_head - 1 - offset) % HISTORY for offset in range(count)
        ]
        values = self.phase_history[indices]
        return max(0.0, float(np.mean(values * values) - np.mean(values) ** 2))

    @staticmethod
    def _estimate_bpm(history: np.ndarray, head: int, count: int,
                      minimum_bpm: float, maximum_bpm: float) -> float:
        if count < 40:
            return 0.0
        start = (head - count) % HISTORY
        values = np.asarray([history[(start + i) % HISTORY] for i in range(count)])
        crossings = int(np.sum((values[:-1] <= 0.0) & (values[1:] > 0.0)))
        duration = (count - 1) / SAMPLE_RATE_HZ
        bpm = crossings * 60.0 / duration if duration else 0.0
        return bpm if minimum_bpm <= bpm <= maximum_bpm else 0.0

    def _person_count(self) -> int:
        if self.top_k.size == 0 or self.motion_energy < self.ambient_threshold:
            return 0
        variances = self._variance()
        groups = np.zeros(4, dtype=np.float64)
        for index, subcarrier in enumerate(self.top_k):
            group = min(3, index * 4 // len(self.top_k))
            groups[group] = max(groups[group], variances[subcarrier])
        strongest = float(np.max(groups))
        threshold = max(self.ambient_threshold * 3.0, strongest * 0.20)
        persons = int(np.sum(groups >= threshold))
        return persons or 1

    def process(self, frame: CsiFrame) -> np.ndarray | None:
        offset = 4 if frame.first_word_invalid and frame.iq.size > 4 else 0
        byte_count = frame.iq.size - offset
        subcarrier_count = min(MAX_SUBCARRIERS, byte_count // 2)
        if subcarrier_count < 8:
            return None
        values = frame.iq[offset : offset + subcarrier_count * 2].astype(np.float64)
        i_values = values[0::2]
        q_values = values[1::2]
        raw_phases = np.arctan2(q_values, i_values)
        phases = np.empty(subcarrier_count, dtype=np.float64)
        for index, raw in enumerate(raw_phases):
            phases[index] = (
                self._unwrap(self.previous_phase[index], float(raw))
                if self.phase_initialized[index]
                else raw
            )
            self.previous_phase[index] = phases[index]
            self.phase_initialized[index] = True
            self._update_welford(index, float(phases[index]))

        self.frame_count += 1
        if self.top_k.size == 0 or self.frame_count % 20 == 1:
            self._update_top_k(subcarrier_count)
        primary = float(phases[int(self.top_k[0])])
        write_index = self.history_head
        self.phase_history[write_index] = primary
        self.breathing_history[write_index] = self.breathing_filter.process(
            primary, 0.10, 0.50
        )
        self.heart_history[write_index] = self.heart_filter.process(primary, 0.80, 2.00)
        self.history_head = (self.history_head + 1) % HISTORY
        self.history_count = min(HISTORY, self.history_count + 1)
        self.motion_energy = self._recent_variance(MOTION_WINDOW)

        if self.history_count >= 2:
            previous_index = (self.history_head - 2) % HISTORY
            velocity = primary - self.phase_history[previous_index]
            acceleration = abs(velocity - self.previous_velocity)
            self.previous_velocity = velocity
            if acceleration > 1.5:
                self.fall_consecutive += 1
                if self.fall_consecutive >= 3:
                    self.fall_hold_until_us = frame.timestamp_us + 5_000_000
                    self.fall_consecutive = 0
            else:
                self.fall_consecutive = 0

        if self.history_count < 40 or self.frame_count % FEATURE_EVERY_FRAMES:
            return None
        breathing = self._estimate_bpm(
            self.breathing_history, self.history_head, self.history_count, 6.0, 30.0
        )
        heart = self._estimate_bpm(
            self.heart_history, self.history_head, self.history_count, 40.0, 120.0
        )
        variances = self._variance()
        mean_variance = float(np.mean(variances[self.top_k])) if self.top_k.size else 0.0
        normalized_motion = np.clip(self.motion_energy / 10.0, 0.0, 1.0)
        return np.asarray(
            [
                normalized_motion,
                normalized_motion,
                np.clip(breathing / 30.0, 0.0, 1.0),
                np.clip(heart / 120.0, 0.0, 1.0),
                np.clip(mean_variance, 0.0, 1.0),
                np.clip(self._person_count() / 4.0, 0.0, 1.0),
                1.0 if frame.timestamp_us < self.fall_hold_until_us else 0.0,
                np.clip((frame.rssi + 100.0) / 100.0, 0.0, 1.0),
            ],
            dtype=np.float32,
        )


def iter_raw_frames(path: Path) -> Iterable[CsiFrame]:
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            if not line.strip():
                continue
            try:
                value = json.loads(line)
                if value.get("type", "raw_csi") != "raw_csi":
                    continue
                yield CsiFrame.from_json(value)
            except (ValueError, TypeError, json.JSONDecodeError) as error:
                raise ValueError(f"{path}:{line_number}: {error}") from error


def session_identity(path: Path) -> tuple[str, str]:
    frame = next(iter(iter_raw_frames(path)), None)
    if frame is None:
        raise ValueError(f"no raw CSI frames in {path}")
    return frame.label, frame.session_id


def discover_and_split(data_dir: Path, seed: int = 42) -> dict[str, list[Path]]:
    grouped: dict[str, list[tuple[str, Path]]] = {label: [] for label in LABELS}
    for path in sorted(data_dir.glob("*.jsonl")):
        label, session = session_identity(path)
        grouped[label].append((session, path))
    randomizer = random.Random(seed)
    split = {"train": [], "validation": [], "test": []}
    for label in LABELS:
        sessions = sorted(grouped[label])
        if len(sessions) < 8:
            raise ValueError(f"{label} needs 8 sessions; found {len(sessions)}")
        randomizer.shuffle(sessions)
        split["train"].extend(path for _, path in sessions[:5])
        split["validation"].append(sessions[5][1])
        split["test"].extend(path for _, path in sessions[6:8])
    return split


def estimate_ambient_threshold(empty_paths: Iterable[Path]) -> float:
    energies: list[float] = []
    for path in empty_paths:
        extractor = FeatureExtractor()
        for frame in iter_raw_frames(path):
            extractor.process(frame)
            if extractor.history_count >= MOTION_WINDOW:
                energies.append(extractor.motion_energy)
    if not energies:
        raise ValueError("cannot estimate ambient threshold without empty CSI frames")
    values = np.asarray(energies, dtype=np.float64)
    return max(1.0e-5, float(values.mean() + 3.0 * values.std()))


def extract_paths(paths: Iterable[Path], ambient_threshold: float) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    features: list[np.ndarray] = []
    labels: list[int] = []
    sessions: list[str] = []
    for path in paths:
        extractor = FeatureExtractor(ambient_threshold)
        for frame in iter_raw_frames(path):
            vector = extractor.process(frame)
            if vector is not None:
                features.append(vector)
                labels.append(LABELS.index(frame.label))
                sessions.append(frame.session_id)
    if not features:
        raise ValueError("no feature vectors extracted")
    return (
        np.stack(features).astype(np.float32),
        np.asarray(labels, dtype=np.int64),
        np.asarray(sessions),
    )


def build_dataset(data_dir: Path, seed: int = 42) -> dict:
    split = discover_and_split(data_dir, seed)
    empty_train = [path for path in split["train"] if session_identity(path)[0] == "empty"]
    threshold = estimate_ambient_threshold(empty_train)
    output: dict = {"ambient_threshold": threshold, "paths": split}
    for name, paths in split.items():
        output[name] = extract_paths(paths, threshold)
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description="Extract RuView-compatible features")
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    dataset = build_dataset(args.data_dir, args.seed)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    payload = {"ambient_threshold": np.asarray(dataset["ambient_threshold"])}
    for split_name in ("train", "validation", "test"):
        x_values, y_values, sessions = dataset[split_name]
        payload[f"x_{split_name}"] = x_values
        payload[f"y_{split_name}"] = y_values
        payload[f"sessions_{split_name}"] = sessions
    np.savez_compressed(args.output, **payload)
    print(f"Wrote {args.output} with ambient threshold {dataset['ambient_threshold']:.8f}")


if __name__ == "__main__":
    main()

