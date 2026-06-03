"""scopeone package."""

from .core import ScopeOne
from .session import RecordingSession

__version__ = "0.1.0"

__all__ = [
    "RecordingSession",
    "ScopeOne",
    "__version__",
]

try:
    from .shm import SharedFrameHeader, frame_to_ndarray, latest_slot_index, parse_frame_header
except ImportError:
    pass
else:
    __all__ += [
        "SharedFrameHeader",
        "frame_to_ndarray",
        "latest_slot_index",
        "parse_frame_header",
    ]
