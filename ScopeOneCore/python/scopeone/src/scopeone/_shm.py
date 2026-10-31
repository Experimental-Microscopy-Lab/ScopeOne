"""Decodes and writes the frame that ScopeOne and the client exchange in shared memory."""

from __future__ import annotations

import struct
import time
from dataclasses import dataclass

import numpy as np

MONO8 = 0
MONO16 = 1

SHARED_FRAME_HEADER_SIZE = 64
SHARED_FRAME_MAX_BYTES = 16 * 2048 * 2048

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
    def payload_nbytes(self) -> int:
        return self.stride * self.height


def parse_frame_header(mapping: memoryview) -> SharedFrameHeader:
    return SharedFrameHeader(*_HEADER_STRUCT.unpack_from(mapping, 0))


# A copy of the frame in the mapping, so the mapping can be closed
def frame_to_ndarray(header: SharedFrameHeader, mapping: memoryview) -> np.ndarray:
    dtype = np.uint16 if header.pixel_format == MONO16 else np.uint8
    payload = mapping[SHARED_FRAME_HEADER_SIZE:SHARED_FRAME_HEADER_SIZE + header.payload_nbytes]
    try:
        array = np.ndarray(shape=(header.height, header.width), dtype=dtype, buffer=payload,
                           strides=(header.stride, np.dtype(dtype).itemsize))
        return np.array(array, copy=True)
    finally:
        payload.release()


def write_ndarray_to_frame(
    mapping: memoryview,
    image: object,
    bits_per_sample: int | None = None,
    frame_index: int = 0,
    timestamp_ns: int | None = None,
    source_roi: tuple[int, int, int, int] | None = None,
) -> tuple[np.ndarray, dict]:
    array = np.asarray(image)
    if array.ndim != 2:
        raise ValueError("ScopeOne frames must be 2D mono images")
    height, width = array.shape

    if bits_per_sample is None:
        bits_per_sample = 8 if array.dtype == np.uint8 else 16
    bits_per_sample = int(bits_per_sample)
    if not 1 <= bits_per_sample <= 16:
        raise ValueError("bits_per_sample must be in the range 1..16")
    if bits_per_sample <= 8:
        pixel_format, bits_per_sample, dtype = MONO8, 8, np.uint8
    else:
        pixel_format, dtype = MONO16, np.uint16

    contiguous = np.ascontiguousarray(np.clip(array, 0, (1 << bits_per_sample) - 1).astype(dtype, copy=False))
    stride = width * np.dtype(dtype).itemsize
    payload_nbytes = stride * height
    if payload_nbytes > SHARED_FRAME_MAX_BYTES:
        raise ValueError("ScopeOne frame payload exceeds the shared mapping capacity")

    source_roi_x, source_roi_y, source_roi_width, source_roi_height = source_roi or (0, 0, 0, 0)
    source_roi_valid = 1 if source_roi_width > 0 and source_roi_height > 0 else 0
    if timestamp_ns is None:
        timestamp_ns = time.time_ns()

    # State 1 while the pixels are written, 2 once the frame is complete
    header_values = (width, height, stride, pixel_format, bits_per_sample, 1, int(frame_index),
                     int(timestamp_ns), source_roi_x, source_roi_y, source_roi_width,
                     source_roi_height, source_roi_valid)
    _HEADER_STRUCT.pack_into(mapping, 0, 1, *header_values)
    payload = mapping[SHARED_FRAME_HEADER_SIZE:SHARED_FRAME_HEADER_SIZE + payload_nbytes]
    try:
        payload[:] = contiguous.tobytes(order="C")
    finally:
        payload.release()
    _HEADER_STRUCT.pack_into(mapping, 0, 2, *header_values)

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
