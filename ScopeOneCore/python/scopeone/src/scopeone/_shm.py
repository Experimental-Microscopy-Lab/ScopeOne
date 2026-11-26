"""Decodes and writes the frame that ScopeOne and the client exchange in shared memory."""

from __future__ import annotations

import struct
import time
from dataclasses import dataclass

import numpy as np

MONO8 = 0
MONO16 = 1

SHARED_FRAME_HEADER_SIZE = 64

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


# The pixels of the shared frame as an array over the mapping itself, not a copy
def frame_array(mapping: memoryview, height: int, width: int, stride: int, dtype) -> np.ndarray:
    return np.ndarray(shape=(height, width), dtype=dtype, buffer=mapping,
                      offset=SHARED_FRAME_HEADER_SIZE, strides=(stride, np.dtype(dtype).itemsize))


# A copy of the frame in the mapping, as the next request overwrites the one shared frame
def frame_to_ndarray(header: SharedFrameHeader, mapping: memoryview) -> np.ndarray:
    dtype = np.uint16 if header.pixel_format == MONO16 else np.uint8
    return frame_array(mapping, header.height, header.width, header.stride, dtype).copy()


# Writes image into dest, clipped to bits_per_sample, in one pass without a temporary copy.
# Returns whether the values had to be clipped or converted.
def store_frame(dest: np.ndarray, image: np.ndarray, bits_per_sample: int) -> bool:
    if image.dtype == dest.dtype and bits_per_sample == dest.dtype.itemsize * 8:
        dest[...] = image
        return False
    np.clip(image, 0, (1 << bits_per_sample) - 1, out=dest, casting="unsafe")
    return True


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

    stride = width * np.dtype(dtype).itemsize
    payload_nbytes = stride * height
    # The mapping the app created is the limit, so the client never has its own copy of it
    if payload_nbytes > len(mapping) - SHARED_FRAME_HEADER_SIZE:
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
    dest = frame_array(mapping, height, width, stride, dtype)
    converted = store_frame(dest, array, bits_per_sample)
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
    return (dest.copy() if converted else array), metadata
