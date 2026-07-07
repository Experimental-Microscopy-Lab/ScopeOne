"""High-level Python API for ScopeOne."""

from __future__ import annotations

from .client import ExternalClient, FrameResult, LOCAL_SERVER_NAME
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

    def process_frame_mapping(
        self,
        camera: str | None = None,
        start_module_index: int | None = None,
        end_module_index: int | None = None,
    ):
        return self._client.process_frame_mapping(
            camera,
            start_module_index,
            end_module_index,
        )

    def latest_raw_frame(self, camera: str):
        return self._client.latest_raw_frame(camera)

    def show_frame_mapping_as_layer(
        self,
        layer_id: str = "python_result",
        name: str = "Python Result",
        camera: str | None = None,
    ):
        return self._client.show_frame_mapping_as_layer(layer_id, name, camera)

    def save_frame_mapping(
        self,
        save_dir: str,
        base_name: str,
        format: str = "tiff",
        compression: bool = False,
        compression_level: int = 6,
        camera: str | None = None,
    ):
        return self._client.save_frame_mapping(
            save_dir,
            base_name,
            format,
            compression,
            compression_level,
            camera,
        )

    def continue_pipeline(self, frame: FrameResult, image: object | None = None):
        if frame.next_module_index is None:
            raise ValueError("FrameResult has no next_module_index")
        frame.write(image)
        return self.process_frame_mapping(
            frame.camera or None,
            start_module_index=frame.next_module_index,
        )

    def show_frame(
        self,
        frame: FrameResult,
        image: object | None = None,
        layer_id: str = "python_result",
        name: str = "Python Result",
    ):
        frame.write(image)
        return self.show_frame_mapping_as_layer(layer_id, name, frame.camera or None)

    def save_frame(
        self,
        frame: FrameResult,
        save_dir: str,
        base_name: str,
        image: object | None = None,
        format: str = "tiff",
        compression: bool = False,
        compression_level: int = 6,
    ):
        frame.write(image)
        return self.save_frame_mapping(
            save_dir,
            base_name,
            format,
            compression,
            compression_level,
            frame.camera or None,
        )

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
