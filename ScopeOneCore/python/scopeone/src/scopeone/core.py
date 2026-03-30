"""High-level Python API for ScopeOne."""

from __future__ import annotations

try:
    from . import _core
except ImportError:
    _core = None


class RecordingSession:
    def __init__(self, native_session) -> None:
        self._session = native_session

    def camera_ids(self):
        return self._session.camera_ids()

    def frame_count(self, camera=None):
        return self._session.frame_count(camera)

    def frame(self, camera: str, index: int):
        return self._session.frame(camera, index)

    def frames(self, camera: str):
        return self._session.frames(camera)

    def save(
        self,
        save_dir: str,
        base_name: str,
        format: str = "tiff",
        compression: bool = False,
        compression_level: int = 6,
    ):
        return self._session.save(
            save_dir,
            base_name,
            format,
            compression,
            compression_level,
        )


class ScopeOne:
    def __init__(self) -> None:
        if _core is None:
            raise RuntimeError("scopeone._core is not available.")
        self._core = _core.ScopeOne()

    def load(self, config_path: str):
        return self._core.load(config_path)

    def unload(self):
        return self._core.unload()

    def camera_ids(self):
        return self._core.camera_ids()

    def record(
        self,
        frame_count: int,
        camera: str = "All",
        timeout_ms: int = 120000,
    ) -> RecordingSession:
        return RecordingSession(
            self._core.record(frame_count, camera, timeout_ms)
        )
