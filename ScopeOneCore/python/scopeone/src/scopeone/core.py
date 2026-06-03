"""High-level Python API for ScopeOne."""

from __future__ import annotations

from .client import ExternalClient, LOCAL_SERVER_NAME
from .session import RecordingSession


class ScopeOne:
    def __init__(self, endpoint: str = "local") -> None:
        server_name = LOCAL_SERVER_NAME if endpoint == "local" else endpoint
        self._client = ExternalClient(server_name)

    @classmethod
    def connect(cls, endpoint: str = "local") -> "ScopeOne":
        return cls(endpoint)

    def load_config(self, config_path: str):
        return self._client.load_config(config_path)

    def unload_config(self):
        self._client.unload_config()

    def camera_ids(self):
        return self._client.camera_ids()

    def start_preview(self, camera: str = "All"):
        self._client.start_preview(camera)

    def stop_preview(self, camera: str = "All"):
        self._client.stop_preview(camera)

    def record(
        self,
        frames: int,
        camera: str = "All",
        timeout_ms: int = 120000,
    ) -> RecordingSession:
        return RecordingSession(
            self._client.record(frames, camera, timeout_ms)
        )
