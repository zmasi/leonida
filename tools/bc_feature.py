"""Vectorised per-block average colour for BC1 and the colour half of BC3.

Endpoints and palette colours are computed in floating point. The averages can differ from GPU readback
when the hardware quantizes the decoded RGB values.
"""
import numpy as np


def _565(c):
    r = ((c >> 11) & 31).astype(np.float32) * (255 / 31)
    g = ((c >> 5) & 63).astype(np.float32) * (255 / 63)
    b = (c & 31).astype(np.float32) * (255 / 31)
    return np.stack([r, g, b], -1)


def _color_average(blocks: np.ndarray, force_four_colors: bool = False) -> np.ndarray:
    c0 = blocks[:, 0].astype(np.uint32) | (blocks[:, 1].astype(np.uint32) << 8)
    c1 = blocks[:, 2].astype(np.uint32) | (blocks[:, 3].astype(np.uint32) << 8)
    p0, p1 = _565(c0), _565(c1)
    four = (force_four_colors | (c0 > c1))[:, None]
    p2 = np.where(four, (2 * p0 + p1) / 3, (p0 + p1) / 2)
    p3 = np.where(four, (p0 + 2 * p1) / 3, 0)
    idx = blocks[:, 4:8].astype(np.uint32)
    bits = idx[:, 0] | (idx[:, 1] << 8) | (idx[:, 2] << 16) | (idx[:, 3] << 24)
    counts = np.zeros((len(blocks), 4), np.float32)
    for k in range(16):
        sel = (bits >> (2 * k)) & 3
        counts[np.arange(len(blocks)), sel] += 1
    pal = np.stack([p0, p1, p2, p3], 1)                   # (N, 4, 3)
    return (pal * counts[:, :, None]).sum(1) / 16


def bc1_average(blocks: np.ndarray) -> np.ndarray:
    """blocks: (N, 8) uint8 -> (N, 3) float average colour of the 16 decoded pixels."""
    if blocks.ndim != 2 or blocks.shape[1] != 8:
        raise ValueError(f"bc1_average expects shape (N, 8), got {blocks.shape}")
    if blocks.dtype != np.uint8:
        raise ValueError(f"bc1_average expects dtype uint8, got {blocks.dtype}")
    return _color_average(blocks)


def block_average(blocks: np.ndarray, bpe: int) -> np.ndarray:
    if bpe not in (8, 16):
        raise ValueError(f"unsupported block size bpe={bpe} (expected 8 or 16)")
    if blocks.ndim != 2 or blocks.shape[1] != bpe:
        raise ValueError(f"block_average(bpe={bpe}) expects shape (N, {bpe}), got {blocks.shape}")
    if blocks.dtype != np.uint8:
        raise ValueError(f"block_average(bpe={bpe}) expects dtype uint8, got {blocks.dtype}")
    if bpe == 8:
        return bc1_average(blocks)
    # BC3 colour uses four entries regardless of endpoint order (EXT_texture_compression_s3tc).
    return _color_average(blocks[:, 8:16], force_four_colors=True)


def smoothness(img: np.ndarray) -> float:
    """Neighbour difference relative to the image's own spread (lower = more coherent)."""
    img = np.asarray(img)
    if img.ndim < 2:
        raise ValueError(f"smoothness expects at least 2 dimensions, got shape {img.shape}")
    if img.size == 0:
        raise ValueError(f"smoothness expects non-empty image, got shape {img.shape}")
    img_f = img if np.issubdtype(img.dtype, np.floating) else img.astype(np.float32)
    dy = float(np.abs(np.diff(img_f, axis=0)).mean()) if img_f.shape[0] > 1 else 0.0
    dx = float(np.abs(np.diff(img_f, axis=1)).mean()) if img_f.shape[1] > 1 else 0.0
    return float((dy + dx) / (float(img_f.std()) + 1e-6))
