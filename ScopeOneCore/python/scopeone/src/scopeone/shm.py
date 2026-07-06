"""Helpers for decoding ScopeOne shared-memory frame slots."""

from __future__ import annotations

import struct
from dataclasses import dataclass

try:
    import numpy as np
except ImportError:
    np = None

MONO8 = 0
MONO16 = 1

SHARED_MEMORY_CONTROL_SIZE = 64
SHARED_FRAME_HEADER_SIZE = 64
SHARED_FRAME_NUM_SLOTS = 12
SHARED_FRAME_MAX_BYTES = 16 * 2048 * 2048
SHARED_FRAME_SLOT_STRIDE = SHARED_FRAME_HEADER_SIZE + SHARED_FRAME_MAX_BYTES

_CONTROL_STRUCT = struct.Struct("<I")
_HEADER_STRUCT = struct.Struct("<IIIIIHHQQiiiiI4x")


@dataclass(frozen=True)
class SharedFrameHeader:
    state: int
    width: int
    height: int
    stride: int
    pixel_format: int
    bits_per_sample: int
    channels: int
    frame_index: int
    timestamp_ns: int
    source_roi_x: int
    source_roi_y: int
    source_roi_width: int
    source_roi_height: int
    source_roi_valid: int

    @property
    def bytes_per_pixel(self) -> int:
        return 2 if self.pixel_format == MONO16 else 1

    @property
    def payload_nbytes(self) -> int:
        return self.stride * self.height

    @property
    def has_source_roi(self) -> bool:
        return self.source_roi_valid != 0 and self.source_roi_width > 0 and self.source_roi_height > 0


def latest_slot_index(control_bytes: bytes | bytearray | memoryview) -> int:
    view = memoryview(control_bytes)
    if len(view) < SHARED_MEMORY_CONTROL_SIZE:
        raise ValueError("Shared memory control block is too small")
    return int(_CONTROL_STRUCT.unpack_from(view, 0)[0])


def parse_frame_header(header_bytes: bytes | bytearray | memoryview) -> SharedFrameHeader:
    view = memoryview(header_bytes)
    if len(view) < SHARED_FRAME_HEADER_SIZE:
        raise ValueError("Shared frame header is too small")
    return SharedFrameHeader(*_HEADER_STRUCT.unpack_from(view, 0))


def frame_to_ndarray(
    header: SharedFrameHeader,
    payload: bytes | bytearray | memoryview,
) -> np.ndarray:
    if np is None:
        raise RuntimeError("numpy is required to decode ScopeOne frames")
    if header.state != 2:
        raise ValueError("Shared frame is not ready")
    if header.pixel_format not in (MONO8, MONO16):
        raise ValueError(f"Unsupported pixel format: {header.pixel_format}")
    if header.channels != 1:
        raise ValueError("ScopeOne frames must use one channel")
    if header.pixel_format == MONO8 and header.bits_per_sample != 8:
        raise ValueError("Mono8 frames must use 8 bits per sample")
    if header.pixel_format == MONO16 and not 1 <= header.bits_per_sample <= 16:
        raise ValueError("Mono16 frames must use 1 to 16 bits per sample")
    if header.width <= 0 or header.height <= 0 or header.stride <= 0:
        raise ValueError("Invalid frame dimensions")

    dtype = np.uint16 if header.pixel_format == MONO16 else np.uint8
    itemsize = np.dtype(dtype).itemsize
    required = header.payload_nbytes
    view = memoryview(payload)
    if len(view) < required:
        raise ValueError("Frame payload is smaller than expected")

    array = np.ndarray(
        shape=(header.height, header.width),
        dtype=dtype,
        buffer=view[:required],
        strides=(header.stride, itemsize),
    )
    return np.array(array, copy=True)
