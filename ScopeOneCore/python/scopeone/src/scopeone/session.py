"""Thin session wrapper for recorded ScopeOne data."""

from __future__ import annotations

from .client import FrameResult


class RecordingSession:
    def __init__(self, native_session) -> None:
        self._session = native_session

    def close(self):
        self._session.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.close()

    def camera_ids(self):
        return self._session.camera_ids()

    def frame_count(self, camera=None):
        return self._session.frame_count(camera)

    def frame(self, camera: str, index: int) -> FrameResult:
        return self._session.frame(camera, index)

    def process_frame(
        self,
        camera: str,
        index: int,
        start_module_index: int | None = None,
        end_module_index: int | None = None,
    ) -> FrameResult:
        return self._session.process_frame(
            camera,
            index,
            start_module_index,
            end_module_index,
        )

    def frames(self, camera: str) -> list[FrameResult]:
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
