"""Backend clients for the public ScopeOne facade."""

from __future__ import annotations

import json
import mmap
import struct
import time
from dataclasses import dataclass

try:
    import pywintypes
    import win32file
except ImportError:
    pywintypes = None
    win32file = None

LOCAL_SERVER_NAME = r"\\.\pipe\ScopeOne.Api.local"
MAX_MESSAGE_BYTES = 256 * 1024


def _metadata_int(metadata: dict, key: str) -> int:
    return int(metadata[key])


def _metadata_bool(metadata: dict, key: str) -> bool:
    return bool(metadata[key])


@dataclass(frozen=True)
class FrameResult:
    image: object
    metadata: dict

    def write(self, image: object | None = None) -> None:
        from .shm import MONO8, MONO16, SHARED_FRAME_HEADER_SIZE, parse_frame_header

        try:
            import numpy as np
        except ImportError as exc:
            raise RuntimeError("numpy is required to write ScopeOne frames") from exc

        mapping_name = str(self.metadata["mappingName"])
        mapping_size = int(self.metadata["mappingSize"])
        with mmap.mmap(-1, mapping_size, tagname=mapping_name, access=mmap.ACCESS_WRITE) as view:
            header = parse_frame_header(view[:SHARED_FRAME_HEADER_SIZE])
            if header.state != 2:
                raise RuntimeError("Shared frame mapping no longer contains a ready FrameResult")
            if header.pixel_format not in (MONO8, MONO16):
                raise ValueError(f"Unsupported pixel format: {header.pixel_format}")
            if header.channels != 1:
                raise ValueError("ScopeOne frames must use one channel")
            if self.metadata["pixelFormat"] == "Mono16":
                expected_pixel_format = MONO16
            elif self.metadata["pixelFormat"] == "Mono8":
                expected_pixel_format = MONO8
            else:
                raise ValueError(f"Unsupported metadata pixel format: {self.metadata['pixelFormat']}")
            if (
                header.width != _metadata_int(self.metadata, "width")
                or header.height != _metadata_int(self.metadata, "height")
                or header.stride != _metadata_int(self.metadata, "stride")
                or header.pixel_format != expected_pixel_format
                or header.bits_per_sample != _metadata_int(self.metadata, "bitsPerSample")
                or header.frame_index != _metadata_int(self.metadata, "frameIndex")
                or header.timestamp_ns != _metadata_int(self.metadata, "timestampNs")
                or header.payload_nbytes != _metadata_int(self.metadata, "payloadBytes")
                or header.source_roi_x != _metadata_int(self.metadata, "sourceRoiX")
                or header.source_roi_y != _metadata_int(self.metadata, "sourceRoiY")
                or header.source_roi_width != _metadata_int(self.metadata, "sourceRoiWidth")
                or header.source_roi_height != _metadata_int(self.metadata, "sourceRoiHeight")
                or bool(header.source_roi_valid) != _metadata_bool(self.metadata, "sourceRoiValid")
            ):
                raise RuntimeError("Shared frame mapping no longer contains this FrameResult")
            dtype = np.uint16 if header.pixel_format == MONO16 else np.uint8
            array = np.asarray(self.image if image is None else image)
            if array.shape != (header.height, header.width):
                raise ValueError("Edited frame shape does not match the shared frame")

            max_value = (1 << header.bits_per_sample) - 1
            contiguous = np.ascontiguousarray(np.clip(array, 0, max_value).astype(dtype, copy=False))
            row_bytes = header.width * contiguous.dtype.itemsize
            if row_bytes > header.stride:
                raise ValueError("Shared frame stride is smaller than one image row")
            if header.payload_nbytes > mapping_size - SHARED_FRAME_HEADER_SIZE:
                raise ValueError("Shared frame payload exceeds the mapping size")
            row_padding = b"\x00" * (header.stride - row_bytes)
            struct.pack_into("<I", view, 0, 1)
            payload = memoryview(view)[SHARED_FRAME_HEADER_SIZE:]
            try:
                for row in range(header.height):
                    begin = row * header.stride
                    payload[begin:begin + row_bytes] = contiguous[row].tobytes(order="C")
                    if row_padding:
                        payload[begin + row_bytes:begin + header.stride] = row_padding
            finally:
                payload.release()
            struct.pack_into("<I", view, 0, 2)


def _add_stage_fields(
    request: dict,
    start_module_index: int | None = None,
    end_module_index: int | None = None,
) -> None:
    if start_module_index is not None and end_module_index is not None:
        raise ValueError("Use either start_module_index or end_module_index")
    if start_module_index is not None:
        request["startModuleIndex"] = int(start_module_index)
    if end_module_index is not None:
        request["endModuleIndex"] = int(end_module_index)


class ExternalClient:
    def __init__(self, server_name: str = LOCAL_SERVER_NAME) -> None:
        if pywintypes is None or win32file is None:
            raise RuntimeError("pywin32 is required for ScopeOne external client.")
        self._handle = self._connect_pipe(server_name)
        self._request({"type": "ping"})

    def load_config(self, config_path: str) -> bool:
        self._request({"type": "load_config", "configPath": config_path})
        return True

    def unload_config(self) -> None:
        self._request({"type": "unload_config"})

    def camera_ids(self) -> list[str]:
        response = self._request({"type": "camera_ids"})
        return list(response.get("cameraIds", []))

    def start_preview(self, camera: str = "All") -> None:
        self._request({"type": "start_preview", "camera": camera})

    def stop_preview(self, camera: str = "All") -> None:
        self._request({"type": "stop_preview", "camera": camera})

    def device_properties(self, device: str, from_cache: bool = True) -> list[dict]:
        response = self._request(
            {
                "type": "device_properties",
                "device": device,
                "fromCache": from_cache,
            }
        )
        return list(response.get("properties", []))

    def device_property_names(self, device: str) -> list[str]:
        response = self._request({"type": "device_property_names", "device": device})
        return list(response.get("names", []))

    def get_property(self, device: str, property: str, from_cache: bool = True) -> str:
        response = self._request(
            {
                "type": "get_property",
                "device": device,
                "property": property,
                "fromCache": from_cache,
            }
        )
        return str(response.get("value", ""))

    def set_property(self, device: str, property: str, value: str) -> None:
        self._request(
            {
                "type": "set_property",
                "device": device,
                "property": property,
                "value": str(value),
            }
        )

    def xy_stage_devices(self) -> list[str]:
        response = self._request({"type": "xy_stage_devices"})
        return list(response.get("devices", []))

    def z_stage_devices(self) -> list[str]:
        response = self._request({"type": "z_stage_devices"})
        return list(response.get("devices", []))

    def current_xy_stage_device(self) -> str:
        response = self._request({"type": "current_xy_stage_device"})
        return str(response.get("device", ""))

    def current_focus_device(self) -> str:
        response = self._request({"type": "current_focus_device"})
        return str(response.get("device", ""))

    def read_xy_position(self, device: str | None = None) -> tuple[float, float]:
        response = self._request(
            {
                "type": "read_xy_position",
                "device": device or "",
            }
        )
        return float(response["x"]), float(response["y"])

    def read_z_position(self, device: str | None = None) -> float:
        response = self._request(
            {
                "type": "read_z_position",
                "device": device or "",
            }
        )
        return float(response["z"])

    def move_xy_relative(self, dx: float, dy: float, device: str | None = None) -> None:
        self._request(
            {
                "type": "move_xy_relative",
                "device": device or "",
                "dx": float(dx),
                "dy": float(dy),
            }
        )

    def move_z_relative(self, dz: float, device: str | None = None) -> None:
        self._request(
            {
                "type": "move_z_relative",
                "device": device or "",
                "dz": float(dz),
            }
        )

    def move_xy_to(self, x: float, y: float, device: str | None = None) -> None:
        self._request(
            {
                "type": "move_xy_to",
                "device": device or "",
                "x": float(x),
                "y": float(y),
            }
        )

    def move_z_to(self, z: float, device: str | None = None) -> None:
        self._request(
            {
                "type": "move_z_to",
                "device": device or "",
                "z": float(z),
            }
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
    ):
        request = {
            "type": "record",
            "frames": frames,
            "camera": camera,
            "timeoutMs": timeout_ms,
            "mdaIntervalMs": float(mda_interval_ms),
        }
        if z_positions is not None:
            request["zPositions"] = [float(z) for z in z_positions]
        if positions is not None:
            request["positions"] = [[float(x), float(y)] for x, y in positions]
        if order is not None:
            request["order"] = [str(axis) for axis in order]
        response = self._request(request)
        return ExternalRecordingSession(self, str(response["sessionId"]))

    def process_frame_mapping(
        self,
        camera: str | None = None,
        start_module_index: int | None = None,
        end_module_index: int | None = None,
    ) -> FrameResult:
        request = {"type": "process_frame_mapping"}
        if camera:
            request["camera"] = camera
        _add_stage_fields(request, start_module_index, end_module_index)
        response = self._request(request)
        return self._frame_result_from_mapping_response(response)

    def latest_raw_frame(self, camera: str) -> FrameResult:
        response = self._request(
            {
                "type": "latest_raw_frame",
                "camera": camera,
            }
        )
        return self._frame_result_from_mapping_response(response)

    def show_frame_mapping_as_layer(
        self,
        layer_id: str = "python_result",
        name: str = "Python Result",
        camera: str | None = None,
    ) -> str:
        request = {
            "type": "show_frame_mapping_as_layer",
            "layerId": layer_id,
            "name": name,
        }
        if camera:
            request["camera"] = camera
        response = self._request(request)
        return str(response["layerKey"])

    @staticmethod
    def _pipe_path(server_name: str) -> str:
        if server_name.startswith("\\\\.\\pipe\\"):
            return server_name
        return rf"\\.\pipe\{server_name}"

    @classmethod
    def _connect_pipe(cls, server_name: str):
        pipe_path = cls._pipe_path(server_name)
        deadline = time.monotonic() + 5.0
        while True:
            try:
                return win32file.CreateFile(
                    pipe_path,
                    win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                    0,
                    None,
                    win32file.OPEN_EXISTING,
                    0,
                    None,
                )
            except pywintypes.error as exc:
                if time.monotonic() >= deadline:
                    raise RuntimeError(
                        f"Failed to connect to ScopeOne server '{server_name}': {exc}"
                    ) from exc
                time.sleep(0.05)

    def _request(self, message: dict):
        payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
        if not payload or len(payload) > MAX_MESSAGE_BYTES:
            raise RuntimeError("ScopeOne control message is invalid or too large")

        framed = struct.pack("<I", len(payload)) + payload
        try:
            win32file.WriteFile(self._handle, framed)
            response_size = self._read_exact(4)
            payload_size = struct.unpack("<I", response_size)[0]
            if payload_size <= 0 or payload_size > MAX_MESSAGE_BYTES:
                raise RuntimeError("ScopeOne control response has invalid size")
            response = json.loads(self._read_exact(payload_size).decode("utf-8"))
        except pywintypes.error as exc:
            raise RuntimeError(f"ScopeOne control request failed: {exc}") from exc

        if not response.get("ok", False):
            error = response.get("error", "ScopeOne request failed")
            raise RuntimeError(error)
        return response

    def _read_exact(self, size: int) -> bytes:
        chunks = bytearray()
        while len(chunks) < size:
            _, data = win32file.ReadFile(self._handle, size - len(chunks))
            if not data:
                raise RuntimeError("ScopeOne control connection closed")
            chunks.extend(data)
        return bytes(chunks)

    @staticmethod
    def _frame_result_from_mapping_response(response: dict) -> FrameResult:
        from .shm import SHARED_FRAME_HEADER_SIZE, frame_to_ndarray, parse_frame_header

        mapping_name = str(response["mappingName"])
        mapping_size = int(response["mappingSize"])
        with mmap.mmap(-1, mapping_size, tagname=mapping_name, access=mmap.ACCESS_READ) as view:
            header = parse_frame_header(view[:SHARED_FRAME_HEADER_SIZE])
            image = frame_to_ndarray(header, view[SHARED_FRAME_HEADER_SIZE:])
        return FrameResult(image=image, metadata=dict(response))


class ExternalRecordingSession:
    def __init__(self, client: ExternalClient, session_id: str) -> None:
        self._client = client
        self._session_id = session_id

    def _info(self) -> dict:
        return self._client._request(
            {
                "type": "session_info",
                "sessionId": self._session_id,
            }
        )

    def camera_ids(self):
        return list(self._info().get("cameraIds", []))

    def frame_count(self, camera=None):
        info = self._info()
        if camera:
            return int(info.get("frameCounts", {}).get(camera, 0))
        return int(info.get("frameCount", 0))

    def frame(self, camera: str, index: int) -> FrameResult:
        response = self._client._request(
            {
                "type": "session_frame",
                "sessionId": self._session_id,
                "camera": camera,
                "index": index,
            }
        )
        return self._client._frame_result_from_mapping_response(response)

    def process_frame(
        self,
        camera: str,
        index: int,
        start_module_index: int | None = None,
        end_module_index: int | None = None,
    ) -> FrameResult:
        request = {
            "type": "session_process_frame",
            "sessionId": self._session_id,
            "camera": camera,
            "index": index,
        }
        _add_stage_fields(request, start_module_index, end_module_index)
        response = self._client._request(request)
        return self._client._frame_result_from_mapping_response(response)

    def frames(self, camera: str) -> list[FrameResult]:
        return [self.frame(camera, index) for index in range(self.frame_count(camera))]

    def save(
        self,
        save_dir: str,
        base_name: str,
        format: str = "tiff",
        compression: bool = False,
        compression_level: int = 6,
    ):
        response = self._client._request(
            {
                "type": "session_save",
                "sessionId": self._session_id,
                "saveDir": save_dir,
                "baseName": base_name,
                "format": format,
                "compression": compression,
                "compressionLevel": compression_level,
            }
        )
        return list(response.get("paths", []))
