"""scopeone package.

``ScopeOne`` controls a running ScopeOne and moves frames in and out of it as numpy arrays.
``scopeone.processing`` goes the other way: ScopeOne runs Python functions as pipeline modules.
"""

from .client import ExperimentSession, FrameResult, RecordingSession, ScopeOne

__version__ = "0.1.0"

__all__ = [
    "ExperimentSession",
    "FrameResult",
    "RecordingSession",
    "ScopeOne",
    "__version__",
]
