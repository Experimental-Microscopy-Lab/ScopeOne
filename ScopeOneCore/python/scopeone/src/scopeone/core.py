"""High-level Python API for ScopeOne."""

from __future__ import annotations

from .client import ExternalClient, FrameResult, LOCAL_SERVER_NAME
from .session import ExperimentSession, RecordingSession


class ScopeOne:
    def __init__(self, endpoint: str = "local") -> None:
        server_name = LOCAL_SERVER_NAME if endpoint == "local" else endpoint
        self._client = ExternalClient(server_name)

    @classmethod
    def connect(cls, endpoint: str = "local") -> "ScopeOne":
        return cls(endpoint)

    def close(self):
        self._client.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.close()

    def load_config(self, config_path: str):
        return self._client.load_config(config_path)

    def unload_config(self):
        self._client.unload_config()

    def version(self):
        return self._client.version()

    def status(self):
        return self._client.status()

    def camera_ids(self):
        return self._client.camera_ids()

    def loaded_devices(self):
        return self._client.loaded_devices()

    def start_preview(self, camera: str = "All"):
        self._client.start_preview(camera)

    def stop_preview(self, camera: str = "All"):
        self._client.stop_preview(camera)

    def list_layers(self):
        return self._client.list_layers()

    def layer_options(self):
        return self._client.layer_options()

    def set_layer_layout(self, layout: str):
        return self._client.set_layer_layout(layout)

    def set_selected_layers(self, layer_keys: list[str]):
        return self._client.set_selected_layers(layer_keys)

    def set_layer_display(
        self,
        layer_key: str,
        visible: bool | None = None,
        opacity_percent: int | None = None,
        gamma: float | None = None,
        colormap: str | None = None,
        blending: str | None = None,
        levels: tuple[int, int, int] | None = None,
    ):
        return self._client.set_layer_display(
            layer_key,
            visible,
            opacity_percent,
            gamma,
            colormap,
            blending,
            levels,
        )

    def move_layer(self, layer_key: str, offset: int):
        return self._client.move_layer(layer_key, offset)

    def config_groups(self):
        return self._client.config_groups()

    def configs(self, group: str):
        return self._client.configs(group)

    def current_config(self, group: str):
        return self._client.current_config(group)

    def set_config(self, group: str, config: str):
        return self._client.set_config(group, config)

    def remove_static_layer(self, layer_key: str):
        self._client.remove_static_layer(layer_key)

    def clear_static_layers(self):
        self._client.clear_static_layers()

    def create_line_markup(
        self,
        layer_key: str,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        label: str = "",
        role: str = "generic",
    ):
        return self._client.create_line_markup(layer_key, x1, y1, x2, y2, label, role)

    def create_rect_markup(
        self,
        layer_key: str,
        x: int,
        y: int,
        width: int,
        height: int,
        label: str = "",
        role: str = "generic",
    ):
        return self._client.create_rect_markup(layer_key, x, y, width, height, label, role)

    def list_markups(self, layer_key: str | None = None):
        return self._client.list_markups(layer_key)

    def remove_markup(self, markup_id: str):
        self._client.remove_markup(markup_id)

    def update_markup(
        self,
        markup_id: str,
        label: str | None = None,
        visible: bool | None = None,
        selected: bool | None = None,
        **geometry,
    ):
        return self._client.update_markup(markup_id, label, visible, selected, **geometry)

    def clear_markups(self, layer_key: str | None = None):
        self._client.clear_markups(layer_key)

    def device_properties(self, device: str, from_cache: bool = True):
        return self._client.device_properties(device, from_cache)

    def device_property_names(self, device: str):
        return self._client.device_property_names(device)

    def get_property(self, device: str, property: str, from_cache: bool = True):
        return self._client.get_property(device, property, from_cache)

    def set_property(self, device: str, property: str, value: str):
        self._client.set_property(device, property, value)

    def read_exposure(self, camera: str = "All"):
        return self._client.read_exposure(camera)

    def set_exposure(self, exposure_ms: float, camera: str = "All"):
        return self._client.set_exposure(exposure_ms, camera)

    def get_roi(self, camera: str):
        return self._client.get_roi(camera)

    def set_roi(self, camera: str, x: int, y: int, width: int, height: int):
        return self._client.set_roi(camera, x, y, width, height)

    def clear_roi(self, camera: str):
        self._client.clear_roi(camera)

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

    def processing_state(self):
        return self._client.processing_state()

    def processing_modules(self):
        return self._client.processing_modules()

    def set_processing_bit_depth(self, bit_depth: int):
        self._client.set_processing_bit_depth(bit_depth)

    def set_realtime_processing(self, enabled: bool):
        self._client.set_realtime_processing(enabled)

    def start_processing(self):
        self.set_realtime_processing(True)

    def stop_processing(self):
        self.set_realtime_processing(False)

    def add_processing_module(self, kind: str | int, parameters: dict | None = None):
        return self._client.add_processing_module(kind, parameters)

    def remove_processing_module(self, index: int):
        self._client.remove_processing_module(index)

    def set_processing_module_parameters(self, index: int, parameters: dict):
        self._client.set_processing_module_parameters(index, parameters)

    def reset_processing_module_state(self, index: int):
        self._client.reset_processing_module_state(index)

    def frame_mapping_info(self):
        return self._client.frame_mapping_info()

    def write_frame_mapping(
        self,
        image: object,
        camera: str = "python",
        bits_per_sample: int | None = None,
        frame_index: int = 0,
        source_roi: tuple[int, int, int, int] | None = None,
    ):
        return self._client.write_frame_mapping(
            image,
            camera,
            bits_per_sample,
            frame_index,
            source_roi,
        )

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

    def process_image(
        self,
        image: object,
        camera: str = "python",
        bits_per_sample: int | None = None,
        start_module_index: int | None = None,
        end_module_index: int | None = None,
    ):
        return self._client.process_image(
            image,
            camera,
            bits_per_sample,
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

    def show_image(
        self,
        image: object,
        layer_id: str = "python_result",
        name: str = "Python Result",
        camera: str = "python",
        bits_per_sample: int | None = None,
    ):
        return self._client.show_image(
            image,
            layer_id,
            name,
            camera,
            bits_per_sample,
        )

    def save_image(
        self,
        image: object,
        save_dir: str,
        base_name: str,
        format: str = "tiff",
        compression: bool = False,
        compression_level: int = 6,
        camera: str = "python",
        bits_per_sample: int | None = None,
    ):
        return self._client.save_image(
            image,
            save_dir,
            base_name,
            format,
            compression,
            compression_level,
            camera,
            bits_per_sample,
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

    def experiment_document(self) -> dict:
        return self._client.experiment_document()

    def validate_experiment(self, document: dict) -> dict:
        return self._client.validate_experiment(document)

    def save_experiment(self, file_path: str, document: dict) -> str:
        return self._client.save_experiment(file_path, document)

    def load_experiment(self, file_path: str) -> dict:
        return self._client.load_experiment(file_path)

    def start_experiment(self, document: dict) -> ExperimentSession:
        return ExperimentSession(self._client.start_experiment(document))

    def experiment_status(self, experiment_id: str) -> dict:
        return self._client.experiment_status(experiment_id)

    def cancel_experiment(self, experiment_id: str) -> dict:
        return self._client.cancel_experiment(experiment_id)

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
