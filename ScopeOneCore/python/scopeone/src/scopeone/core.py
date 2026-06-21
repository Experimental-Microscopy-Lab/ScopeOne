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

    def device_properties(self, device: str, from_cache: bool = True):
        return self._client.device_properties(device, from_cache)

    def device_property_names(self, device: str):
        return self._client.device_property_names(device)

    def get_property(self, device: str, property: str, from_cache: bool = True):
        return self._client.get_property(device, property, from_cache)

    def set_property(self, device: str, property: str, value: str):
        self._client.set_property(device, property, value)

    def xy_stage_devices(self):
        return self._client.xy_stage_devices()

    def z_stage_devices(self):
        return self._client.z_stage_devices()

    def current_xy_stage_device(self):
        return self._client.current_xy_stage_device()

    def current_focus_device(self):
        return self._client.current_focus_device()

    def read_xy_position(self, device: str | None = None):
        return self._client.read_xy_position(device)

    def read_z_position(self, device: str | None = None):
        return self._client.read_z_position(device)

    def move_xy_relative(self, dx: float, dy: float, device: str | None = None):
        self._client.move_xy_relative(dx, dy, device)

    def move_z_relative(self, dz: float, device: str | None = None):
        self._client.move_z_relative(dz, device)

    def move_xy_to(self, x: float, y: float, device: str | None = None):
        self._client.move_xy_to(x, y, device)

    def move_z_to(self, z: float, device: str | None = None):
        self._client.move_z_to(z, device)

    def record(
        self,
        frames: int,
        camera: str = "All",
        timeout_ms: int = 120000,
        mda_interval_ms: float = 0.0,
        z_positions: list[float] | None = None,
        positions: list[tuple[float, float]] | None = None,
        order: list[str] | None = None,
    ) -> RecordingSession:
        return RecordingSession(
            self._client.record(
                frames,
                camera,
                timeout_ms,
                mda_interval_ms,
                z_positions,
                positions,
                order,
            )
        )
