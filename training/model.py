from __future__ import annotations

from pathlib import Path

import torch
from huggingface_hub import hf_hub_download
from safetensors.torch import load_file
from torch import nn
from torch.nn import functional as F


LABELS = ("empty", "still", "moving")
HF_REPOSITORY = "ruvnet/wifi-densepose-pretrained"
HF_FILENAME = "csi-embed-v2.safetensors"


class CsiEncoder(nn.Module):
    """RuView v2 8 -> 64 -> 128 contrastive CSI encoder."""

    def __init__(self) -> None:
        super().__init__()
        self.w1 = nn.Linear(8, 64)
        self.bn1 = nn.BatchNorm1d(64)
        self.w2 = nn.Linear(64, 128)
        self.bn2 = nn.BatchNorm1d(128)

    def forward(self, inputs: torch.Tensor) -> torch.Tensor:
        hidden = F.gelu(self.bn1(self.w1(inputs)))
        return F.normalize(self.bn2(self.w2(hidden)), dim=-1)


class RuViewClassifier(nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.encoder = CsiEncoder()
        self.head = nn.Linear(128, len(LABELS))

    def forward(self, inputs: torch.Tensor) -> torch.Tensor:
        return self.head(self.encoder(inputs))


def load_pretrained_encoder(
    encoder: CsiEncoder,
    cache_dir: Path | None = None,
    checkpoint: Path | None = None,
) -> Path:
    if checkpoint is None:
        downloaded = hf_hub_download(
            repo_id=HF_REPOSITORY,
            filename=HF_FILENAME,
            cache_dir=str(cache_dir) if cache_dir else None,
        )
        checkpoint = Path(downloaded)
    state = load_file(str(checkpoint))
    encoder.load_state_dict(state, strict=True)
    return checkpoint

