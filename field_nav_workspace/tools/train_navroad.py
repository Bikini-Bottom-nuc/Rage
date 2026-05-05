#!/usr/bin/env python3
"""Train and export a tiny grayscale road segmentation model."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image

try:
    import torch
    import torch.nn as nn
    import torch.nn.functional as F
    from torch.utils.data import DataLoader, Dataset
except ImportError as exc:  # pragma: no cover - runtime dependency guard
    raise SystemExit("PyTorch is required for training this model.") from exc


class RoadDataset(Dataset):
    def __init__(self, data_dir: Path, split: str, augment: bool = False) -> None:
        self.data_dir = data_dir
        self.names = [
            line.strip()
            for line in (data_dir / "splits" / f"{split}.txt").read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
        self.augment = augment

    def __len__(self) -> int:
        return len(self.names)

    def __getitem__(self, index: int) -> tuple[torch.Tensor, torch.Tensor]:
        name = self.names[index]
        image = np.asarray(Image.open(self.data_dir / "images" / f"{name}.png").convert("L"), dtype=np.float32)
        mask = np.asarray(Image.open(self.data_dir / "masks" / f"{name}.png").convert("L"), dtype=np.float32)
        image = image / 255.0
        mask = (mask > 127).astype(np.float32)
        if self.augment and torch.rand(()) < 0.5:
            image = np.ascontiguousarray(image[:, ::-1])
            mask = np.ascontiguousarray(mask[:, ::-1])
        if self.augment:
            gain = float(torch.empty(()).uniform_(0.85, 1.15))
            bias = float(torch.empty(()).uniform_(-0.08, 0.08))
            image = np.clip(image * gain + bias, 0.0, 1.0)
        return torch.from_numpy(image[None, ...]), torch.from_numpy(mask[None, ...])


class ConvBNAct(nn.Module):
    def __init__(self, in_channels: int, out_channels: int, stride: int = 1) -> None:
        super().__init__()
        self.block = nn.Sequential(
            nn.Conv2d(in_channels, out_channels, 3, stride=stride, padding=1, bias=False),
            nn.BatchNorm2d(out_channels),
            nn.ReLU(inplace=True),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.block(x)


class TinyNavRoadNet(nn.Module):
    """Small stride-4 segmentation network for 1x480x640 input."""

    def __init__(self) -> None:
        super().__init__()
        self.stem = nn.Sequential(ConvBNAct(1, 12), ConvBNAct(12, 12))
        self.down1 = nn.Sequential(ConvBNAct(12, 24, stride=2), ConvBNAct(24, 24))
        self.down2 = nn.Sequential(ConvBNAct(24, 40, stride=2), ConvBNAct(40, 40))
        self.context = nn.Sequential(
            nn.Conv2d(40, 40, 3, padding=2, dilation=2, groups=40, bias=False),
            nn.BatchNorm2d(40),
            nn.ReLU(inplace=True),
            nn.Conv2d(40, 40, 1, bias=False),
            nn.BatchNorm2d(40),
            nn.ReLU(inplace=True),
        )
        self.head = nn.Conv2d(40, 1, 1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = self.stem(x)
        x = self.down1(x)
        x = self.down2(x)
        x = self.context(x)
        return self.head(x)


def dice_loss(logits: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
    target_small = F.interpolate(target, size=logits.shape[-2:], mode="nearest")
    prob = torch.sigmoid(logits)
    inter = (prob * target_small).sum(dim=(1, 2, 3))
    denom = prob.sum(dim=(1, 2, 3)) + target_small.sum(dim=(1, 2, 3))
    return 1.0 - ((2.0 * inter + 1.0) / (denom + 1.0)).mean()


def batch_iou(logits: torch.Tensor, target: torch.Tensor) -> float:
    target_small = F.interpolate(target, size=logits.shape[-2:], mode="nearest") > 0.5
    pred = torch.sigmoid(logits) > 0.5
    inter = (pred & target_small).sum(dim=(1, 2, 3)).float()
    union = (pred | target_small).sum(dim=(1, 2, 3)).float().clamp_min(1.0)
    return float((inter / union).mean().cpu())


def run_epoch(model, loader, optimizer, device: torch.device) -> dict[str, float]:
    training = optimizer is not None
    model.train(training)
    total_loss = 0.0
    total_iou = 0.0
    count = 0
    for image, mask in loader:
        image = image.to(device)
        mask = mask.to(device)
        with torch.set_grad_enabled(training):
            logits = model(image)
            target_small = F.interpolate(mask, size=logits.shape[-2:], mode="nearest")
            loss = F.binary_cross_entropy_with_logits(logits, target_small) + dice_loss(logits, mask)
            if training:
                optimizer.zero_grad(set_to_none=True)
                loss.backward()
                optimizer.step()
        batch = image.size(0)
        total_loss += float(loss.detach().cpu()) * batch
        total_iou += batch_iou(logits.detach(), mask) * batch
        count += batch
    return {"loss": total_loss / max(count, 1), "iou": total_iou / max(count, 1)}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", required=True, type=Path)
    parser.add_argument("--run-dir", default=Path("field_nav_workspace/runs/navroad_tiny"), type=Path)
    parser.add_argument("--epochs", default=80, type=int)
    parser.add_argument("--batch-size", default=8, type=int)
    parser.add_argument("--lr", default=1e-3, type=float)
    parser.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    parser.add_argument("--num-workers", default=0, type=int)
    return parser.parse_args()


def export_onnx(model: nn.Module, path: Path, device: torch.device) -> None:
    model.eval()
    dummy = torch.zeros(1, 1, 480, 640, device=device)
    torch.onnx.export(
        model,
        dummy,
        path,
        input_names=["image"],
        output_names=["road_logits"],
        opset_version=11,
        do_constant_folding=True,
        dynamo=False,
    )


def main() -> None:
    args = parse_args()
    if args.epochs < 1:
        raise SystemExit("--epochs must be >= 1")
    if args.batch_size < 1:
        raise SystemExit("--batch-size must be >= 1")

    args.run_dir.mkdir(parents=True, exist_ok=True)
    device = torch.device(args.device)
    train_set = RoadDataset(args.data_dir, "train", augment=True)
    val_set = RoadDataset(args.data_dir, "val", augment=False)
    if len(train_set) == 0:
        raise SystemExit("train split is empty")
    if len(val_set) == 0:
        raise SystemExit("val split is empty")

    train_loader = DataLoader(train_set, batch_size=args.batch_size, shuffle=True, num_workers=args.num_workers)
    val_loader = DataLoader(val_set, batch_size=args.batch_size, shuffle=False, num_workers=args.num_workers)

    model = TinyNavRoadNet().to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=max(args.epochs, 1))
    best_iou = -math.inf
    history: list[dict] = []

    for epoch in range(1, args.epochs + 1):
        train_metrics = run_epoch(model, train_loader, optimizer, device)
        val_metrics = run_epoch(model, val_loader, None, device)
        scheduler.step()
        row = {"epoch": epoch, "train": train_metrics, "val": val_metrics}
        history.append(row)
        print(json.dumps(row, ensure_ascii=False))
        if val_metrics["iou"] > best_iou:
            best_iou = val_metrics["iou"]
            torch.save({"model": model.state_dict(), "epoch": epoch, "val_iou": best_iou}, args.run_dir / "best.pt")

    torch.save({"model": model.state_dict(), "epoch": args.epochs}, args.run_dir / "last.pt")
    (args.run_dir / "history.json").write_text(json.dumps(history, indent=2), encoding="utf-8")

    best = torch.load(args.run_dir / "best.pt", map_location=device)
    model.load_state_dict(best["model"])
    export_onnx(model, args.run_dir / "navroad_640x480.onnx", device)
    print(f"best val IoU: {best_iou:.4f}")
    print(f"exported ONNX: {args.run_dir / 'navroad_640x480.onnx'}")


if __name__ == "__main__":
    main()
