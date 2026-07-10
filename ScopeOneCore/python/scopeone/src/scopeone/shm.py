"""Helpers for decoding and writing ScopeOne shared-memory frame slots."""

from __future__ import annotations

import struct
import time
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
    try:
        if len(view) < SHARED_MEMORY_CONTROL_SIZE:
            raise ValueError("Shared memory control block is too small")
        return int(_CONTROL_STRUCT.unpack_from(view, 0)[0])
    finally:
        view.release()


def parse_frame_header(header_bytes: bytes | bytearray | memoryview) -> SharedFrameHeader:
    view = memoryview(header_bytes)
    try:
        if len(view) < SHARED_FRAME_HEADER_SIZE:
            raise ValueError("Shared frame header is too small")
        return SharedFrameHeader(*_HEADER_STRUCT.unpack_from(view, 0))
    finally:
        view.release()


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
    frame_view = None
    try:
        if len(view) < required:
            raise ValueError("Frame payload is smaller than expected")

        frame_view = view[:required]
        array = np.ndarray(
            shape=(header.height, header.width),
            dtype=dtype,
            buffer=frame_view,
            strides=(header.stride, itemsize),
        )
        return np.array(array, copy=True)
    finally:
        if frame_view is not None:
            frame_view.release()
        view.release()


def write_ndarray_to_frame(
    view: bytes | bytearray | memoryview,
    image: object,
    bits_per_sample: int | None = None,
    frame_index: int = 0,
    timestamp_ns: int | None = None,
    source_roi: tuple[int, int, int, int] | None = None,
) -> tuple[np.ndarray, dict]:
    if np is None:
        raise RuntimeError("numpy is required to write ScopeOne frames")

    array = np.asarray(image)
    if array.ndim != 2:
        raise ValueError("ScopeOne frames must be 2D mono images")
    height, width = array.shape
    if width <= 0 or height <= 0:
        raise ValueError("ScopeOne frame dimensions must be positive")

    if bits_per_sample is None:
        bits_per_sample = 8 if array.dtype == np.uint8 else 16
    bits_per_sample = int(bits_per_sample)
    if bits_per_sample <= 8:
        pixel_format = MONO8
        bits_per_sample = 8
        dtype = np.uint8
    elif bits_per_sample <= 16:
        pixel_format = MONO16
        dtype = np.uint16
    else:
        raise ValueError("bits_per_sample must be in the range 1..16")

    max_value = (1 << bits_per_sample) - 1
    contiguous = np.ascontiguousarray(np.clip(array, 0, max_value).astype(dtype, copy=False))
    stride = width * np.dtype(dtype).itemsize
    payload_nbytes = stride * height
    if payload_nbytes > SHARED_FRAME_MAX_BYTES:
        raise ValueError("ScopeOne frame payload exceeds the shared mapping capacity")

    if source_roi is None:
        source_roi_x = source_roi_y = source_roi_width = source_roi_height = 0
        source_roi_valid = 0
    else:
        source_roi_x, source_roi_y, source_roi_width, source_roi_height = (int(v) for v in source_roi)
        source_roi_valid = 1 if source_roi_width > 0 and source_roi_height > 0 else 0

    if timestamp_ns is None:
        timestamp_ns = time.time_ns()

    mapping = memoryview(view)
    try:
        required = SHARED_FRAME_HEADER_SIZE + payload_nbytes
        if len(mapping) < required:
            raise ValueError("Shared frame mapping is smaller than the frame payload")

        header_values = (
            1,
            width,
            height,
            stride,
            pixel_format,
            bits_per_sample,
            1,
            int(frame_index),
            int(timestamp_ns),
            source_roi_x,
            source_roi_y,
            source_roi_width,
            source_roi_height,
            source_roi_valid,
        )
        _HEADER_STRUCT.pack_into(mapping, 0, *header_values)
        payload = mapping[SHARED_FRAME_HEADER_SIZE:SHARED_FRAME_HEADER_SIZE + payload_nbytes]
        try:
            payload[:] = contiguous.tobytes(order="C")
        finally:
            payload.release()
        _HEADER_STRUCT.pack_into(mapping, 0, 2, *header_values[1:])
    finally:
        mapping.release()

    metadata = {
        "width": width,
        "height": height,
        "stride": stride,
        "payloadBytes": str(payload_nbytes),
        "pixelFormat": "Mono16" if pixel_format == MONO16 else "Mono8",
        "bitsPerSample": bits_per_sample,
        "frameIndex": str(int(frame_index)),
        "timestampNs": str(int(timestamp_ns)),
        "sourceRoiX": source_roi_x,
        "sourceRoiY": source_roi_y,
        "sourceRoiWidth": source_roi_width,
        "sourceRoiHeight": source_roi_height,
        "sourceRoiValid": bool(source_roi_valid),
    }
    return np.array(contiguous, copy=True), metadata
