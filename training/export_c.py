from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

import numpy as np
import torch

from model import LABELS, RuViewClassifier


def fold_linear_bn(linear: torch.nn.Linear, bn: torch.nn.BatchNorm1d) -> tuple[np.ndarray, np.ndarray]:
    weight = linear.weight.detach().cpu().numpy().astype(np.float64)
    bias = linear.bias.detach().cpu().numpy().astype(np.float64)
    gamma = bn.weight.detach().cpu().numpy().astype(np.float64)
    beta = bn.bias.detach().cpu().numpy().astype(np.float64)
    running_mean = bn.running_mean.detach().cpu().numpy().astype(np.float64)
    running_var = bn.running_var.detach().cpu().numpy().astype(np.float64)
    factor = gamma / np.sqrt(running_var + bn.eps)
    return (weight * factor[:, None]).astype(np.float32), (
        beta + (bias - running_mean) * factor
    ).astype(np.float32)


def quantize_rows(weights: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    maximum = np.max(np.abs(weights), axis=1)
    scales = np.where(maximum > 1.0e-12, maximum / 127.0, 1.0).astype(np.float32)
    quantized = np.clip(np.rint(weights / scales[:, None]), -127, 127).astype(np.int8)
    return quantized, scales


def dynamic_quantize(values: np.ndarray) -> tuple[np.ndarray, float]:
    maximum = float(np.max(np.abs(values)))
    scale = maximum / 127.0 if maximum > 1.0e-12 else 1.0
    return np.clip(np.rint(values / scale), -127, 127).astype(np.int8), scale


def quantized_forward(
    raw_features: np.ndarray,
    input_mean: np.ndarray,
    input_std: np.ndarray,
    w1: np.ndarray,
    w1_scale: np.ndarray,
    b1: np.ndarray,
    w2: np.ndarray,
    w2_scale: np.ndarray,
    b2: np.ndarray,
    head: np.ndarray,
    head_scale: np.ndarray,
    head_bias: np.ndarray,
) -> np.ndarray:
    standardized = (raw_features - input_mean) / input_std
    outputs: list[np.ndarray] = []
    for vector in standardized:
        vector_q, vector_scale = dynamic_quantize(vector)
        hidden = (w1.astype(np.int32) @ vector_q.astype(np.int32)).astype(np.float32)
        hidden = hidden * (w1_scale * vector_scale) + b1
        hidden = 0.5 * hidden * (
            1.0 + np.tanh(0.7978845608 * (hidden + 0.044715 * hidden**3))
        )
        hidden_q, hidden_scale = dynamic_quantize(hidden)
        embedding = (w2.astype(np.int32) @ hidden_q.astype(np.int32)).astype(np.float32)
        embedding = embedding * (w2_scale * hidden_scale) + b2
        embedding /= max(float(np.linalg.norm(embedding)), 1.0e-12)
        embedding_q, embedding_scale = dynamic_quantize(embedding)
        logits = (head.astype(np.int32) @ embedding_q.astype(np.int32)).astype(np.float32)
        outputs.append(logits * (head_scale * embedding_scale) + head_bias)
    return np.stack(outputs)


def format_int_array(name: str, values: np.ndarray, declaration: str) -> str:
    flat = values.reshape(-1)
    lines = []
    for start in range(0, len(flat), 24):
        lines.append("    " + ", ".join(str(int(value)) for value in flat[start : start + 24]))
    return f"static const {declaration} {name}[{len(flat)}] = {{\n" + ",\n".join(lines) + "\n};\n"


def format_float_array(name: str, values: np.ndarray) -> str:
    flat = values.reshape(-1)
    lines = []
    for start in range(0, len(flat), 8):
        literals = []
        for value in flat[start : start + 8]:
            literal = f"{float(value):.9g}"
            if "." not in literal and "e" not in literal.lower():
                literal += ".0"
            literals.append(literal + "f")
        lines.append(
            "    " + ", ".join(literals)
        )
    return f"static constexpr float {name}[{len(flat)}] = {{\n" + ",\n".join(lines) + "\n};\n"


def build_header(checkpoint: dict, model: RuViewClassifier) -> tuple[str, float]:
    input_mean = checkpoint["input_mean"].numpy().astype(np.float32)
    input_std = checkpoint["input_std"].numpy().astype(np.float32)
    w1_float, b1 = fold_linear_bn(model.encoder.w1, model.encoder.bn1)
    w2_float, b2 = fold_linear_bn(model.encoder.w2, model.encoder.bn2)
    head_float = model.head.weight.detach().cpu().numpy().astype(np.float32)
    head_bias = model.head.bias.detach().cpu().numpy().astype(np.float32)
    w1, w1_scale = quantize_rows(w1_float)
    w2, w2_scale = quantize_rows(w2_float)
    head, head_scale = quantize_rows(head_float)

    verification = checkpoint["verification_features"].numpy().astype(np.float32)
    with torch.no_grad():
        standardized = (verification - input_mean) / input_std
        float_predictions = model(torch.from_numpy(standardized)).argmax(dim=1).numpy()
    quantized_logits = quantized_forward(
        verification, input_mean, input_std,
        w1, w1_scale, b1, w2, w2_scale, b2, head, head_scale, head_bias,
    )
    agreement = float(np.mean(float_predictions == quantized_logits.argmax(axis=1)))

    digest = hashlib.sha256()
    for value in (w1, w1_scale, b1, w2, w2_scale, b2, head, head_scale, head_bias,
                  input_mean, input_std):
        digest.update(np.ascontiguousarray(value).tobytes())
    model_hash = digest.hexdigest()
    test_metrics = checkpoint["test_metrics"]

    pieces = [
        "#pragma once\n\n#include <stdint.h>\n\n",
        "// Generated by training/export_c.py. Do not edit by hand.\n",
        "static constexpr bool kRuvModelReady = true;\n",
        "static constexpr uint32_t kRuvModelVersion = 1;\n",
        "static constexpr float kRuvConfidenceThreshold = 0.65f;\n",
        f'static constexpr char kRuvModelSha256[] = "{model_hash}";\n',
        f"static constexpr float kRuvHeldoutMacroF1 = {float(test_metrics['macro_f1']):.9e}f;\n",
        f"static constexpr float kRuvHeldoutMovingRecall = {float(test_metrics['moving_recall']):.9e}f;\n",
        f"static constexpr float kRuvQuantizedAgreement = {agreement:.9e}f;\n\n",
        format_float_array("kRuvInputMean", input_mean),
        format_float_array("kRuvInputStd", input_std),
        "\n",
        format_int_array("kRuvW1", w1, "int8_t"),
        format_float_array("kRuvW1Scale", w1_scale),
        format_float_array("kRuvB1", b1),
        "\n",
        format_int_array("kRuvW2", w2, "int8_t"),
        format_float_array("kRuvW2Scale", w2_scale),
        format_float_array("kRuvB2", b2),
        "\n",
        format_int_array("kRuvHeadW", head, "int8_t"),
        format_float_array("kRuvHeadScale", head_scale),
        format_float_array("kRuvHeadBias", head_bias),
        '\nstatic constexpr const char* kRuvClassLabels[3] = {"empty", "still", "moving"};\n',
    ]
    return "".join(pieces), agreement


def main() -> None:
    parser = argparse.ArgumentParser(description="Export a trained RuView checkpoint for ESP32")
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("../firmware/receiver/include/model_data.h"),
    )
    parser.add_argument("--allow-below-target", action="store_true")
    args = parser.parse_args()

    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    if tuple(checkpoint["labels"]) != LABELS:
        raise SystemExit(f"unexpected labels: {checkpoint['labels']}")
    if not checkpoint.get("accepted", False) and not args.allow_below_target:
        raise SystemExit(
            "checkpoint did not meet macro-F1/moving-recall acceptance targets; "
            "use --allow-below-target only for controlled experiments"
        )
    model = RuViewClassifier()
    model.load_state_dict(checkpoint["state_dict"], strict=True)
    model.eval()
    header, agreement = build_header(checkpoint, model)
    if agreement < 0.98 and not args.allow_below_target:
        raise SystemExit(f"quantized agreement {agreement:.2%} is below 98%")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header, encoding="utf-8", newline="\n")
    print(f"Wrote {args.output}; float/int8 class agreement={agreement:.2%}")


if __name__ == "__main__":
    main()
