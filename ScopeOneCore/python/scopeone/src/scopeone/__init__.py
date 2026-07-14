"""scopeone package."""

from .client import FrameResult
from .core import ScopeOne
from .session import ExperimentSession, RecordingSession
from .shm import (
    SharedFrameHeader,
    frame_to_ndarray,
    latest_slot_index,
    parse_frame_header,
    write_ndarray_to_frame,
)

__version__ = "0.1.0"

__all__ = [
    "FrameResult",
    "ExperimentSession",
    "RecordingSession",
    "ScopeOne",
    "SharedFrameHeader",
    "frame_to_ndarray",
    "latest_slot_index",
    "parse_frame_header",
    "write_ndarray_to_frame",
    "__version__",
]
