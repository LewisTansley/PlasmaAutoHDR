"""Tiny depthwise-separable U-Net for realtime chroma map inference."""

from __future__ import annotations

import torch
import torch.nn as nn
import torch.nn.functional as F


class DepthwiseBlock(nn.Module):
    def __init__(self, channels: int):
        super().__init__()
        self.dw = nn.Conv2d(channels, channels, 3, padding=1, groups=channels, bias=False)
        self.pw = nn.Conv2d(channels, channels, 1, bias=False)
        self.act = nn.ReLU(inplace=True)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.act(self.pw(self.dw(x)))


class ChromaNet(nn.Module):
    """Input: NCHW RGB [0,1+]. Output: NCHW 4-channel chroma map."""

    def __init__(self, base: int = 16):
        super().__init__()
        self.enc1 = nn.Sequential(
            nn.Conv2d(3, base, 3, padding=1, bias=False),
            nn.ReLU(inplace=True),
            DepthwiseBlock(base),
        )
        self.enc2 = nn.Sequential(
            nn.Conv2d(base, base * 2, 3, stride=2, padding=1, bias=False),
            nn.ReLU(inplace=True),
            DepthwiseBlock(base * 2),
        )
        self.mid = DepthwiseBlock(base * 2)
        self.dec2 = nn.Sequential(
            nn.ConvTranspose2d(base * 2, base, 4, stride=2, padding=1, bias=False),
            nn.ReLU(inplace=True),
            DepthwiseBlock(base),
        )
        self.head = nn.Conv2d(base * 2, 4, 1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        e1 = self.enc1(x)
        e2 = self.enc2(e1)
        m = self.mid(e2)
        d = self.dec2(m)
        if d.shape[-2:] != e1.shape[-2:]:
            d = F.interpolate(d, size=e1.shape[-2:], mode="bilinear", align_corners=False)
        out = self.head(torch.cat([e1, d], dim=1))
        strength = torch.sigmoid(out[:, 0:1])
        sat = torch.tanh(out[:, 1:2]) * 0.5 + 0.5
        hue = torch.tanh(out[:, 2:3]) * 0.5 + 0.5
        ci = torch.sigmoid(out[:, 3:4])
        return torch.cat([strength, sat, hue, ci], dim=1)


def y_lock_loss(rgb_before: torch.Tensor, rgb_after: torch.Tensor) -> torch.Tensor:
    """Penalize CIE Y drift (Rec.709 luma weights)."""
    w = rgb_before.new_tensor([0.2126, 0.7152, 0.0722]).view(1, 3, 1, 1)
    y0 = (rgb_before * w).sum(dim=1, keepdim=True)
    y1 = (rgb_after * w).sum(dim=1, keepdim=True)
    return F.l1_loss(y1, y0)
