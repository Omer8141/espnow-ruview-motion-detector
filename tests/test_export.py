from __future__ import annotations

import numpy as np
import torch

from export_c import build_header, fold_linear_bn
from model import LABELS, RuViewClassifier


def test_batchnorm_folding_matches_eval_path() -> None:
    torch.manual_seed(7)
    model = RuViewClassifier().eval()
    inputs = torch.randn(12, 8)
    with torch.no_grad():
        expected = model.encoder.bn1(model.encoder.w1(inputs)).numpy()
    weight, bias = fold_linear_bn(model.encoder.w1, model.encoder.bn1)
    actual = inputs.numpy() @ weight.T + bias
    np.testing.assert_allclose(actual, expected, rtol=1.0e-5, atol=1.0e-5)


def test_header_export_contains_ready_model_and_has_high_agreement() -> None:
    torch.manual_seed(11)
    model = RuViewClassifier().eval()
    verification = torch.randn(256, 8) * 0.2
    checkpoint = {
        "input_mean": torch.zeros(8),
        "input_std": torch.ones(8),
        "verification_features": verification,
        "test_metrics": {"macro_f1": 0.9, "moving_recall": 0.95},
        "labels": list(LABELS),
    }
    header, agreement = build_header(checkpoint, model)
    assert agreement >= 0.98
    assert "kRuvModelReady = true" in header
    assert "kRuvW2[8192]" in header
    assert "kRuvHeadW[384]" in header

