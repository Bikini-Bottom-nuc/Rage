#!/usr/bin/env python3
"""Train a v2 lightweight grayscale road model with skip fusion."""

from __future__ import annotations

import argparse
import json
import math
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

try:
    import torch
    import torch.nn as nn
    import torch.nn.functional as F
    from torch.utils.data import DataLoader, Dataset
except ImportError as exc:  # pragma: no cover - runtime dependency guard
    raise SystemExit("PyTorch is required for training this model.") from exc


def set_seed(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)


class RoadDataset(Dataset):
    def __init__(self, data_dir: Path, split: str, augment: bool = False, return_name: bool = False) -> None:
        self.data_dir = data_dir
        self.names = [
            line.strip()
            for line in (data_dir / "splits" / f"{split}.txt").read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
        self.augment = augment
        self.return_name = return_name

    def __len__(self) -> int:
        return len(self.names)

    def _augment(self, image: np.ndarray, mask: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        if random.random() < 0.5:
            image = np.ascontiguousarray(image[:, ::-1])
            mask = np.ascontiguousarray(mask[:, ::-1])

        if random.random() < 0.9:
            mean = float(image.mean())
            contrast = random.uniform(0.75, 1.30)
            gain = random.uniform(0.82, 1.18)
            bias = random.uniform(-0.10, 0.10)
            image = (image - mean) * contrast + mean
            image = image * gain + bias

        if random.random() < 0.45:
            gamma = random.uniform(0.70, 1.45)
            image = np.power(np.clip(image, 0.0, 1.0), gamma)

        if random.random() < 0.35:
            h, w = image.shape
            xs = np.linspace(0.0, 1.0, w, dtype=np.float32)
            center = random.uniform(0.0, 1.0)
            width = random.uniform(0.18, 0.45)
            strength = random.uniform(0.18, 0.45)
            profile = np.clip(1.0 - np.abs(xs - center) / width, 0.0, 1.0)
            if random.random() < 0.5:
                profile = profile[::-1]
            image = image * (1.0 - strength * profile[None, :])

        if random.random() < 0.18:
            radius = random.uniform(0.35, 1.10)
            pil = Image.fromarray(np.clip(image * 255.0, 0, 255).astype(np.uint8))
            image = np.asarray(pil.filter(ImageFilter.GaussianBlur(radius=radius)), dtype=np.float32) / 255.0

        image = np.clip(image, 0.0, 1.0)
        return image, mask

    def __getitem__(self, index: int):
        name = self.names[index]
        image = np.asarray(Image.open(self.data_dir / "images" / f"{name}.png").convert("L"), dtype=np.float32) / 255.0
        mask = np.asarray(Image.open(self.data_dir / "masks" / f"{name}.png").convert("L"), dtype=np.float32)
        mask = (mask > 127).astype(np.float32)
        if self.augment:
            image, mask = self._augment(image, mask)
        image_tensor = torch.from_numpy(np.ascontiguousarray(image[None, ...]))
        mask_tensor = torch.from_numpy(np.ascontiguousarray(mask[None, ...]))
        if self.return_name:
            return name, image_tensor, mask_tensor
        return image_tensor, mask_tensor


class ConvBNAct(nn.Module):
    def __init__(self, in_channels: int, out_channels: int, stride: int = 1, dilation: int = 1) -> None:
        super().__init__()
        self.block = nn.Sequential(
            nn.Conv2d(
                in_channels,
                out_channels,
                3,
                stride=stride,
                padding=dilation,
                dilation=dilation,
                bias=False,
            ),
            nn.BatchNorm2d(out_channels),
            nn.ReLU(inplace=True),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.block(x)


class DepthwiseSeparable(nn.Module):
    def __init__(self, channels: int, dilation: int = 1) -> None:
        super().__init__()
        self.block = nn.Sequential(
            nn.Conv2d(channels, channels, 3, padding=dilation, dilation=dilation, groups=channels, bias=False),
            nn.BatchNorm2d(channels),
            nn.ReLU(inplace=True),
            nn.Conv2d(channels, channels, 1, bias=False),
            nn.BatchNorm2d(channels),
            nn.ReLU(inplace=True),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.block(x)


def scaled_channels(base: int, width_mult: float) -> int:
    return max(4, int(round(base * width_mult)))


class SkipFusionNavRoadNet(nn.Module):
    """Stride-4 segmentation net for 1x480x640 grayscale input."""

    def __init__(self, width_mult: float = 1.0) -> None:
        super().__init__()
        c1 = scaled_channels(12, width_mult)
        c2 = scaled_channels(24, width_mult)
        c3 = scaled_channels(40, width_mult)
        cf = scaled_channels(32, width_mult)
        self.width_mult = float(width_mult)
        self.stem = nn.Sequential(ConvBNAct(1, c1), ConvBNAct(c1, c1))
        self.down1 = nn.Sequential(ConvBNAct(c1, c2, stride=2), DepthwiseSeparable(c2))
        self.down2 = nn.Sequential(ConvBNAct(c2, c3, stride=2), DepthwiseSeparable(c3))
        self.context = nn.Sequential(DepthwiseSeparable(c3, dilation=2), DepthwiseSeparable(c3, dilation=3))
        self.fuse = nn.Sequential(ConvBNAct(c1 + c2 + c3, cf), DepthwiseSeparable(cf))
        self.head = nn.Conv2d(cf, 1, 1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        stem = self.stem(x)
        down1 = self.down1(stem)
        down2 = self.down2(down1)
        context = self.context(down2)
        stem_skip = F.avg_pool2d(stem, kernel_size=4, stride=4)
        down1_skip = F.avg_pool2d(down1, kernel_size=2, stride=2)
        fused = torch.cat([context, down1_skip, stem_skip], dim=1)
        return self.head(self.fuse(fused))


def dice_loss(logits: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
    prob = torch.sigmoid(logits)
    inter = (prob * target).sum(dim=(1, 2, 3))
    denom = prob.sum(dim=(1, 2, 3)) + target.sum(dim=(1, 2, 3))
    return 1.0 - ((2.0 * inter + 1.0) / (denom + 1.0)).mean()


def focal_bce_loss(logits: torch.Tensor, target: torch.Tensor, gamma: float = 2.0) -> torch.Tensor:
    bce = F.binary_cross_entropy_with_logits(logits, target, reduction="none")
    prob = torch.sigmoid(logits)
    pt = prob * target + (1.0 - prob) * (1.0 - target)
    alpha = target * 0.60 + (1.0 - target) * 0.40
    return (alpha * (1.0 - pt).pow(gamma) * bce).mean()


def segmentation_loss(logits: torch.Tensor, mask: torch.Tensor) -> torch.Tensor:
    target = F.interpolate(mask, size=logits.shape[-2:], mode="nearest")
    bce = F.binary_cross_entropy_with_logits(logits, target)
    return 0.70 * bce + dice_loss(logits, target) + 0.40 * focal_bce_loss(logits, target)


def row_centers(mask: np.ndarray, threshold: float = 0.5) -> dict[int, float]:
    h, w = mask.shape
    step = max(1, h // 32)
    min_span = max(2, w // 40)
    points: dict[int, float] = {}
    for y in range(h - 1, int(h * 0.35), -step):
        xs = np.where(mask[y] > threshold)[0]
        if xs.size >= min_span:
            points[y] = float(xs.mean())
    return points


def row_centers_best_segment(mask: np.ndarray, threshold: float = 0.5) -> dict[int, float]:
    h, w = mask.shape
    step = max(1, h // 32)
    min_span = max(2, w // 40)
    points: dict[int, float] = {}
    for y in range(h - 1, int(h * 0.35), -step):
        row = mask[y]
        best_start = -1
        best_end = -1
        best_sum = 0.0
        start = -1
        running_sum = 0.0
        for x, value in enumerate(row):
            if value > threshold:
                if start < 0:
                    start = x
                    running_sum = 0.0
                running_sum += float(value)
            ended = value <= threshold or x == w - 1
            if start >= 0 and ended:
                end = x - 1 if value <= threshold else x
                span = end - start + 1
                if span >= min_span and running_sum > best_sum:
                    best_start = start
                    best_end = end
                    best_sum = running_sum
                start = -1
        if best_start >= 0:
            points[y] = float((best_start + best_end) * 0.5)
    return points


def centerline_errors(pred: np.ndarray, target: np.ndarray, mode: str = "all") -> tuple[float | None, float | None]:
    if mode == "best_segment":
        pred_points = row_centers_best_segment(pred)
        target_points = row_centers_best_segment(target)
    else:
        pred_points = row_centers(pred)
        target_points = row_centers(target)
    common = sorted(set(pred_points) & set(target_points))
    if len(common) < 4:
        return None, None
    errors = [abs(pred_points[y] - target_points[y]) for y in common]
    bottom_y = max(common)
    return float(np.mean(errors)), float(abs(pred_points[bottom_y] - target_points[bottom_y]))


def batch_metrics(logits: torch.Tensor, mask: torch.Tensor, threshold: float = 0.5) -> dict[str, float]:
    prob = torch.sigmoid(logits)
    prob_full = F.interpolate(prob, size=mask.shape[-2:], mode="bilinear", align_corners=False)
    pred = prob_full > threshold
    target = mask > 0.5
    inter = (pred & target).sum(dim=(1, 2, 3)).float()
    union = (pred | target).sum(dim=(1, 2, 3)).float().clamp_min(1.0)
    ious = (inter / union).detach().cpu().numpy().tolist()
    center_errors: list[float] = []
    bottom_errors: list[float] = []
    invalid = 0
    pred_np = pred.squeeze(1).detach().cpu().numpy().astype(np.float32)
    target_np = target.squeeze(1).detach().cpu().numpy().astype(np.float32)
    for pred_item, target_item in zip(pred_np, target_np):
        center_error, bottom_error = centerline_errors(pred_item, target_item)
        if center_error is None or bottom_error is None:
            invalid += 1
        else:
            center_errors.append(center_error)
            bottom_errors.append(bottom_error)
    return {
        "iou_sum": float(np.sum(ious)),
        "center_error_sum": float(np.sum(center_errors)),
        "bottom_error_sum": float(np.sum(bottom_errors)),
        "center_count": float(len(center_errors)),
        "invalid": float(invalid),
    }


def run_epoch(model, loader, optimizer, device: torch.device) -> dict[str, float | None]:
    training = optimizer is not None
    model.train(training)
    total_loss = 0.0
    total_iou = 0.0
    total_center_error = 0.0
    total_bottom_error = 0.0
    center_count = 0.0
    invalid = 0.0
    count = 0
    for image, mask in loader:
        image = image.to(device)
        mask = mask.to(device)
        with torch.set_grad_enabled(training):
            logits = model(image)
            loss = segmentation_loss(logits, mask)
            if training:
                optimizer.zero_grad(set_to_none=True)
                loss.backward()
                optimizer.step()
        batch = image.size(0)
        metrics = batch_metrics(logits.detach(), mask)
        total_loss += float(loss.detach().cpu()) * batch
        total_iou += metrics["iou_sum"]
        total_center_error += metrics["center_error_sum"]
        total_bottom_error += metrics["bottom_error_sum"]
        center_count += metrics["center_count"]
        invalid += metrics["invalid"]
        count += batch
    return {
        "loss": total_loss / max(count, 1),
        "iou": total_iou / max(count, 1),
        "mean_center_error_px": total_center_error / center_count if center_count else None,
        "mean_bottom_error_px": total_bottom_error / center_count if center_count else None,
        "invalid_centerline_samples": int(invalid),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", required=True, type=Path)
    parser.add_argument("--run-dir", default=Path("field_nav_workspace/runs/navroad_v2"), type=Path)
    parser.add_argument("--epochs", default=120, type=int)
    parser.add_argument("--batch-size", default=8, type=int)
    parser.add_argument("--lr", default=8e-4, type=float)
    parser.add_argument("--weight-decay", default=1e-4, type=float)
    parser.add_argument("--patience", default=15, type=int)
    parser.add_argument("--min-delta", default=1e-4, type=float)
    parser.add_argument("--width-mult", default=1.0, type=float)
    parser.add_argument("--seed", default=20260427, type=int)
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


def is_better(row: dict, best_score: float, args: argparse.Namespace) -> tuple[bool, float]:
    val = row["val"]
    center_error = val["mean_center_error_px"] if val["mean_center_error_px"] is not None else 640.0
    score = float(val["iou"]) - 0.05 * (float(center_error) / 640.0)
    return score > best_score + args.min_delta, score


def main() -> None:
    args = parse_args()
    if args.epochs < 1:
        raise SystemExit("--epochs must be >= 1")
    if args.batch_size < 1:
        raise SystemExit("--batch-size must be >= 1")
    if args.patience < 1:
        raise SystemExit("--patience must be >= 1")
    if args.width_mult <= 0.0:
        raise SystemExit("--width-mult must be > 0")

    set_seed(args.seed)
    args.run_dir.mkdir(parents=True, exist_ok=True)
    device = torch.device(args.device)
    train_set = RoadDataset(args.data_dir, "train", augment=True)
    val_set = RoadDataset(args.data_dir, "val", augment=False)
    if len(train_set) == 0:
        raise SystemExit("train split is empty")
    if len(val_set) == 0:
        raise SystemExit("val split is empty")

    train_loader = DataLoader(
        train_set,
        batch_size=args.batch_size,
        shuffle=True,
        num_workers=args.num_workers,
        pin_memory=device.type == "cuda",
    )
    val_loader = DataLoader(
        val_set,
        batch_size=args.batch_size,
        shuffle=False,
        num_workers=args.num_workers,
        pin_memory=device.type == "cuda",
    )

    model = SkipFusionNavRoadNet(width_mult=args.width_mult).to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=args.weight_decay)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=max(args.epochs, 1))
    best_score = -math.inf
    bad_epochs = 0
    history: list[dict] = []
    config = {
        "width_mult": args.width_mult,
        "input_shape": [1, 480, 640],
        "output_stride": 4,
        "loss": "0.70*bce + dice + 0.40*focal_bce",
    }

    for epoch in range(1, args.epochs + 1):
        train_metrics = run_epoch(model, train_loader, optimizer, device)
        val_metrics = run_epoch(model, val_loader, None, device)
        scheduler.step()
        row = {
            "epoch": epoch,
            "lr": optimizer.param_groups[0]["lr"],
            "train": train_metrics,
            "val": val_metrics,
        }
        history.append(row)
        print(json.dumps(row, ensure_ascii=False))
        better, score = is_better(row, best_score, args)
        if better:
            best_score = score
            bad_epochs = 0
            torch.save(
                {
                    "model": model.state_dict(),
                    "epoch": epoch,
                    "val": val_metrics,
                    "score": best_score,
                    "config": config,
                },
                args.run_dir / "best.pt",
            )
        else:
            bad_epochs += 1
            if bad_epochs >= args.patience:
                print(f"early stopping at epoch {epoch}, best score {best_score:.5f}")
                break

    torch.save({"model": model.state_dict(), "epoch": history[-1]["epoch"], "config": config}, args.run_dir / "last.pt")
    (args.run_dir / "history.json").write_text(json.dumps(history, indent=2), encoding="utf-8")

    best = torch.load(args.run_dir / "best.pt", map_location=device)
    model.load_state_dict(best["model"])
    export_onnx(model, args.run_dir / "navroad_640x480.onnx", device)
    summary = {
        "best_epoch": best["epoch"],
        "best_val": best["val"],
        "best_score": best["score"],
        "onnx": str(args.run_dir / "navroad_640x480.onnx"),
        "config": config,
    }
    (args.run_dir / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
