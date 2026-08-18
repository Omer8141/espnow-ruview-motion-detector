from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np
import pytest

from feature_pipeline import (
    CsiFrame,
    FeatureExtractor,
    discover_and_split,
)


def synthetic_frame(index: int, *, invalid_first_word: bool = False,
                    label: str = "moving", session: str = "moving_near_01") -> CsiFrame:
    time_seconds = index / 20.0
    iq: list[int] = [0, 0, 0, 0] if invalid_first_word else []
    for subcarrier in range(64):
        phase = (
            0.25 * math.sin(2.0 * math.pi * 0.30 * time_seconds)
            + 0.04 * subcarrier
            + (0.35 * math.sin(2.0 * math.pi * 1.2 * time_seconds) if label == "moving" else 0)
        )
        iq.extend(
            [
                int(np.clip(round(40.0 * math.cos(phase)), -128, 127)),
                int(np.clip(round(40.0 * math.sin(phase)), -128, 127)),
            ]
        )
    return CsiFrame(
        timestamp_us=index * 50_000,
        sequence=index,
        source_mac="24:6F:28:11:22:33",
        channel=6,
        rssi=-52,
        first_word_invalid=invalid_first_word,
        iq=np.asarray(iq, dtype=np.int8),
        label=label,
        session_id=session,
    )


@pytest.mark.parametrize("invalid_first_word", [False, True])
def test_feature_vector_is_bounded_and_periodic(invalid_first_word: bool) -> None:
    extractor = FeatureExtractor(ambient_threshold=1.0e-5)
    vectors = [
        vector
        for index in range(180)
        if (vector := extractor.process(
            synthetic_frame(index, invalid_first_word=invalid_first_word)
        )) is not None
    ]
    assert len(vectors) > 20
    matrix = np.stack(vectors)
    assert matrix.shape[1] == 8
    assert np.all(np.isfinite(matrix))
    assert np.all(matrix >= 0.0)
    assert np.all(matrix <= 1.0)
    np.testing.assert_allclose(matrix[:, 0], matrix[:, 1])


def test_json_parser_rejects_bad_length() -> None:
    value = {
        "type": "raw_csi",
        "timestamp_us": 1,
        "sequence": 1,
        "source_mac": "00:11:22:33:44:55",
        "channel": 6,
        "rssi": -50,
        "csi_length": 8,
        "first_word_invalid": False,
        "iq": [1, 2],
        "label": "empty",
        "session_id": "empty_01",
    }
    with pytest.raises(ValueError, match="csi_length"):
        CsiFrame.from_json(value)


def test_session_split_is_5_1_2_per_class(tmp_path: Path) -> None:
    for label in ("empty", "still", "moving"):
        for index in range(8):
            session = f"{label}_{index:02d}"
            frame = synthetic_frame(0, label=label, session=session)
            value = {
                "type": "raw_csi",
                "timestamp_us": frame.timestamp_us,
                "sequence": frame.sequence,
                "source_mac": frame.source_mac,
                "channel": frame.channel,
                "rssi": frame.rssi,
                "csi_length": int(frame.iq.size),
                "first_word_invalid": frame.first_word_invalid,
                "iq": frame.iq.astype(int).tolist(),
                "label": label,
                "session_id": session,
            }
            (tmp_path / f"train_{label}_{session}.jsonl").write_text(
                json.dumps(value) + "\n", encoding="utf-8"
            )
    split = discover_and_split(tmp_path, seed=42)
    assert len(split["train"]) == 15
    assert len(split["validation"]) == 3
    assert len(split["test"]) == 6

