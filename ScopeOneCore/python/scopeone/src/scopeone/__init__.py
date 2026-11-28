"""scopeone package.

``ScopeOne`` controls a running ScopeOne and moves frames in and out of it as numpy arrays.
``scopeone.processing`` goes the other way: ScopeOne runs Python functions as pipeline modules.
"""

# MAJOR.MINOR of the ScopeOne this client speaks to. The release workflow writes the full
# version of the app here, and the client refuses an app of another MAJOR.MINOR.
__version__ = "1.5"

from .client import Frame, RecordingSession, ScopeOne

__all__ = [
    "Frame",
    "RecordingSession",
    "ScopeOne",
    "__version__",
]
