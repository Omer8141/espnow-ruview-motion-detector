from __future__ import annotations

import argparse
import copy
import hashlib
import json
import random
from pathlib import Path

import numpy as np
import torch
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import accuracy_score, confusion_matrix, f1_score, recall_score
from torch import nn
from torch.utils.data import DataLoader, Dataset

from feature_pipeline import LABELS, build_dataset
from model import RuViewClassifier, load_pretrained_encoder


class FeatureDataset(Dataset):
    def __init__(self, features: np.ndarray, labels: np.ndarray, augment: bool) -> None:
        self.features = torch.from_numpy(features.astype(np.float32))
        self.labels = torch.from_numpy(labels.astype(np.int64))
        self.augment = augment

    def __len__(self) -> int:
        return len(self.labels)

    def __getitem__(self, index: int) -> tuple[torch.Tensor, torch.Tensor]:
        vector = self.features[index].clone()
        if self.augment:
            vector += torch.randn_like(vector) * 0.02
            if torch.rand(()) < 0.10:
                vector[torch.randint(0, vector.numel(), ())] = 0.0
            vector[7] *= 1.0 + (torch.rand(()) - 0.5) * 0.06
        return vector, self.labels[index]


def seed_everything(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)
    torch.use_deterministic_algorithms(True, warn_only=True)


def metrics(labels: np.ndarray, predictions: np.ndarray) -> dict:
    return {
        "accuracy": float(accuracy_score(labels, predictions)),
        "macro_f1": float(f1_score(labels, predictions, average="macro", zero_division=0)),
        "moving_recall": float(
            recall_score(labels == 2, predictions == 2, zero_division=0)
        ),
        "confusion_matrix": confusion_matrix(
            labels, predictions, labels=np.arange(len(LABELS))
        ).tolist(),
    }


@torch.no_grad()
def evaluate(model: nn.Module, features: np.ndarray, labels: np.ndarray,
             device: torch.device) -> tuple[dict, np.ndarray]:
    model.eval()
    tensor = torch.from_numpy(features.astype(np.float32)).to(device)
    logits: list[torch.Tensor] = []
    for start in range(0, tensor.shape[0], 1024):
        logits.append(model(tensor[start : start + 1024]).cpu())
    predictions = torch.cat(logits).argmax(dim=1).numpy()
    return metrics(labels, predictions), predictions


def configure_stage(model: RuViewClassifier, stage: str) -> None:
    for parameter in model.parameters():
        parameter.requires_grad = False
    for parameter in model.head.parameters():
        parameter.requires_grad = True
    if stage in {"second", "all"}:
        for module in (model.encoder.w2, model.encoder.bn2):
            for parameter in module.parameters():
                parameter.requires_grad = True
    if stage == "all":
        for parameter in model.encoder.parameters():
            parameter.requires_grad = True


def apply_training_modes(model: RuViewClassifier, stage: str) -> None:
    model.train()
    if stage == "head":
        model.encoder.eval()
    elif stage == "second":
        model.encoder.w1.eval()
        model.encoder.bn1.eval()
        model.encoder.w2.train()
        model.encoder.bn2.train()


def train_stage(
    model: RuViewClassifier,
    stage: str,
    train_loader: DataLoader,
    validation_x: np.ndarray,
    validation_y: np.ndarray,
    class_weights: torch.Tensor,
    epochs: int,
    learning_rate: float,
    device: torch.device,
) -> tuple[dict, dict]:
    configure_stage(model, stage)
    optimizer = torch.optim.AdamW(
        [parameter for parameter in model.parameters() if parameter.requires_grad],
        lr=learning_rate,
        weight_decay=1.0e-4,
    )
    criterion = nn.CrossEntropyLoss(weight=class_weights)
    best_state = copy.deepcopy(model.state_dict())
    best_metrics, _ = evaluate(model, validation_x, validation_y, device)
    stale_epochs = 0

    for epoch in range(epochs):
        apply_training_modes(model, stage)
        total_loss = 0.0
        total_examples = 0
        for inputs, labels in train_loader:
            inputs = inputs.to(device)
            labels = labels.to(device)
            optimizer.zero_grad(set_to_none=True)
            loss = criterion(model(inputs), labels)
            loss.backward()
            optimizer.step()
            total_loss += float(loss.detach()) * labels.numel()
            total_examples += labels.numel()
        current_metrics, _ = evaluate(model, validation_x, validation_y, device)
        print(
            f"stage={stage} epoch={epoch + 1:02d} "
            f"loss={total_loss / max(total_examples, 1):.5f} "
            f"val_macro_f1={current_metrics['macro_f1']:.4f}"
        )
        if current_metrics["macro_f1"] > best_metrics["macro_f1"] + 1.0e-5:
            best_metrics = current_metrics
            best_state = copy.deepcopy(model.state_dict())
            stale_epochs = 0
        else:
            stale_epochs += 1
            if stale_epochs >= 6:
                break
    model.load_state_dict(best_state)
    return best_state, best_metrics


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description="Fine-tune RuView for three CSI states")
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("artifacts/ruview_motion.pt"))
    parser.add_argument("--encoder", type=Path)
    parser.add_argument("--cache-dir", type=Path, default=Path("artifacts/hf-cache"))
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--batch-size", type=int, default=64)
    args = parser.parse_args()

    seed_everything(args.seed)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    dataset = build_dataset(args.data_dir, args.seed)
    train_x, train_y, _ = dataset["train"]
    validation_x, validation_y, _ = dataset["validation"]
    test_x, test_y, _ = dataset["test"]

    input_mean = train_x.mean(axis=0).astype(np.float32)
    input_std = train_x.std(axis=0).astype(np.float32)
    input_std[input_std < 1.0e-6] = 1.0
    standardize = lambda values: ((values - input_mean) / input_std).astype(np.float32)
    train_standardized = standardize(train_x)
    validation_standardized = standardize(validation_x)
    test_standardized = standardize(test_x)

    benchmark = LogisticRegression(
        max_iter=1000, class_weight="balanced", random_state=args.seed
    )
    benchmark.fit(train_standardized, train_y)
    benchmark_metrics = metrics(
        test_y, benchmark.predict(test_standardized)
    )
    print("Raw-feature logistic benchmark:", json.dumps(benchmark_metrics))

    model = RuViewClassifier()
    pretrained_path = load_pretrained_encoder(
        model.encoder, args.cache_dir, args.encoder
    )
    model.to(device)
    class_counts = np.bincount(train_y, minlength=len(LABELS)).astype(np.float32)
    weights = class_counts.sum() / (len(LABELS) * np.maximum(class_counts, 1.0))
    class_weights = torch.from_numpy(weights).to(device)
    train_loader = DataLoader(
        FeatureDataset(train_standardized, train_y, augment=True),
        batch_size=args.batch_size,
        shuffle=True,
        drop_last=len(train_y) >= args.batch_size,
        generator=torch.Generator().manual_seed(args.seed),
    )

    _, validation_metrics = train_stage(
        model, "head", train_loader, validation_standardized, validation_y,
        class_weights, 30, 1.0e-3, device
    )
    completed_stage = "head"
    if validation_metrics["macro_f1"] < 0.85:
        _, validation_metrics = train_stage(
            model, "second", train_loader, validation_standardized, validation_y,
            class_weights, 20, 1.0e-4, device
        )
        completed_stage = "second"
    if validation_metrics["macro_f1"] < 0.85:
        _, validation_metrics = train_stage(
            model, "all", train_loader, validation_standardized, validation_y,
            class_weights, 20, 5.0e-5, device
        )
        completed_stage = "all"

    test_metrics, _ = evaluate(model, test_standardized, test_y, device)
    accepted = (
        test_metrics["macro_f1"] >= 0.85
        and test_metrics["moving_recall"] >= 0.90
    )
    print("Validation:", json.dumps(validation_metrics))
    print("Held-out test:", json.dumps(test_metrics))
    print("ACCEPTED" if accepted else "NOT ACCEPTED: collect or improve data before deployment")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    checkpoint = {
        "format_version": 1,
        "state_dict": {key: value.detach().cpu() for key, value in model.state_dict().items()},
        "input_mean": torch.from_numpy(input_mean),
        "input_std": torch.from_numpy(input_std),
        "labels": list(LABELS),
        "ambient_threshold": float(dataset["ambient_threshold"]),
        "validation_metrics": validation_metrics,
        "test_metrics": test_metrics,
        "benchmark_metrics": benchmark_metrics,
        "accepted": accepted,
        "completed_stage": completed_stage,
        "seed": args.seed,
        "pretrained_source": str(pretrained_path),
        "pretrained_sha256": sha256_file(pretrained_path),
        "verification_features": torch.from_numpy(test_x.astype(np.float32)),
        "verification_labels": torch.from_numpy(test_y.astype(np.int64)),
        "session_paths": {
            name: [str(path) for path in dataset["paths"][name]]
            for name in ("train", "validation", "test")
        },
    }
    torch.save(checkpoint, args.output)
    print(f"Saved candidate checkpoint to {args.output}")


if __name__ == "__main__":
    main()

