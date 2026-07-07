"""Backend clients for the public ScopeOne facade.

The control channel and frame transport are platform-specific: on Windows the
server exposes a named pipe plus a named file mapping, and on Linux/Unix a
QLocalServer unix socket plus a POSIX shared-memory object under /dev/shm. The
JSON request protocol and the shared-frame layout are identical on both.
"""

from __future__ import annotations

import json
import mmap
import os
import struct
import time
from dataclasses import dataclass

_IS_WINDOWS = os.name == "nt"

if _IS_WINDOWS:
    try:
        import pywintypes
        import win32file
    except ImportError:
        pywintypes = None
        win32file = None
else:
    import socket

# Windows: named pipe. Unix: QLocalServer turns this name into a socket file
# under the temp directory (e.g. /tmp/ScopeOne.Api.local).
WIN_LOCAL_SERVER_NAME = r"\\.\pipe\ScopeOne.Api.local"
UNIX_LOCAL_SERVER_NAME = "ScopeOne.Api.local"
LOCAL_SERVER_NAME = WIN_LOCAL_SERVER_NAME if _IS_WINDOWS else UNIX_LOCAL_SERVER_NAME

MAX_MESSAGE_BYTES = 256 * 1024
_CONNECT_TIMEOUT_S = 5.0


class _WindowsPipeTransport:
    """Control channel + frame mapping over Windows named pipe / file mapping."""

    def __init__(self, server_name: str) -> None:
        if pywintypes is None or win32file is None:
            raise RuntimeError("pywin32 is required for ScopeOne external client.")
        self._handle = self._connect(server_name)

    @staticmethod
    def _pipe_path(server_name: str) -> str:
        if server_name.startswith("\\\\.\\pipe\\"):
            return server_name
        return rf"\\.\pipe\{server_name}"

    def _connect(self, server_name: str):
        pipe_path = self._pipe_path(server_name)
        deadline = time.monotonic() + _CONNECT_TIMEOUT_S
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

    def send(self, data: bytes) -> None:
        try:
            win32file.WriteFile(self._handle, data)
        except pywintypes.error as exc:
            raise RuntimeError(f"ScopeOne control request failed: {exc}") from exc

    def recv_exact(self, size: int) -> bytes:
        chunks = bytearray()
        while len(chunks) < size:
            try:
                _, data = win32file.ReadFile(self._handle, size - len(chunks))
            except pywintypes.error as exc:
                raise RuntimeError(f"ScopeOne control request failed: {exc}") from exc
            if not data:
                raise RuntimeError("ScopeOne control connection closed")
            chunks.extend(data)
        return bytes(chunks)

    def open_frame(self, mapping_name: str, mapping_size: int, write: bool = False):
        access = mmap.ACCESS_WRITE if write else mmap.ACCESS_READ
        return mmap.mmap(-1, mapping_size, tagname=mapping_name, access=access)

    def close(self) -> None:
        if self._handle is not None:
            try:
                win32file.CloseHandle(self._handle)
            except Exception:
                pass
            self._handle = None


class _UnixSocketTransport:
    """Control channel over unix socket + frame read from /dev/shm."""

    def __init__(self, server_name: str) -> None:
        self._sock = self._connect(server_name)

    @staticmethod
    def _server_path(server_name: str) -> str:
        if os.path.isabs(server_name):
            return server_name
        # Matches Qt's QDir::tempPath() (honors $TMPDIR, else /tmp).
        return os.path.join(os.environ.get("TMPDIR", "/tmp"), server_name)

    def _connect(self, server_name: str):
        path = self._server_path(server_name)
        deadline = time.monotonic() + _CONNECT_TIMEOUT_S
        while True:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                sock.connect(path)
                return sock
            except OSError as exc:
                sock.close()
                if time.monotonic() >= deadline:
                    raise RuntimeError(
                        f"Failed to connect to ScopeOne server '{path}': {exc}"
                    ) from exc
                time.sleep(0.05)

    def send(self, data: bytes) -> None:
        try:
            self._sock.sendall(data)
        except OSError as exc:
            raise RuntimeError(f"ScopeOne control request failed: {exc}") from exc

    def recv_exact(self, size: int) -> bytes:
        chunks = bytearray()
        while len(chunks) < size:
            try:
                data = self._sock.recv(size - len(chunks))
            except OSError as exc:
                raise RuntimeError(f"ScopeOne control request failed: {exc}") from exc
            if not data:
                raise RuntimeError("ScopeOne control connection closed")
            chunks.extend(data)
        return bytes(chunks)

    def open_frame(self, mapping_name: str, mapping_size: int, write: bool = False):
        # The server publishes the frame as a POSIX shm object; it appears at
        # /dev/shm/<name>. mapping_name is the bare object name.
        path = mapping_name if os.path.isabs(mapping_name) else f"/dev/shm/{mapping_name}"
        fd = os.open(path, os.O_RDWR if write else os.O_RDONLY)
        try:
            prot = mmap.PROT_READ | (mmap.PROT_WRITE if write else 0)
            return mmap.mmap(fd, mapping_size, prot=prot)
        finally:
            os.close(fd)

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except Exception:
                pass
            self._sock = None


def _make_transport(server_name: str):
    if _IS_WINDOWS:
        return _WindowsPipeTransport(server_name)
    return _UnixSocketTransport(server_name)



def _metadata_int(metadata: dict, key: str) -> int:
    return int(metadata[key])


def _metadata_bool(metadata: dict, key: str) -> bool:
    return bool(metadata[key])


@dataclass
class FrameResult:
    image: object
    metadata: dict
    _transport: object

    @property
    def camera(self) -> str:
        return str(self.metadata.get("camera", ""))

    @property
    def width(self) -> int:
        return _metadata_int(self.metadata, "width")

    @property
    def height(self) -> int:
        return _metadata_int(self.metadata, "height")

    @property
    def pixel_format(self) -> str:
        return str(self.metadata.get("pixelFormat", ""))

    @property
    def bits_per_sample(self) -> int:
        return _metadata_int(self.metadata, "bitsPerSample")

    @property
    def frame_index(self) -> int:
        return _metadata_int(self.metadata, "frameIndex")

    @property
    def timestamp_ns(self) -> int:
        return _metadata_int(self.metadata, "timestampNs")

    @property
    def source_roi(self) -> tuple[int, int, int, int] | None:
        if not _metadata_bool(self.metadata, "sourceRoiValid"):
            return None
        return (
            _metadata_int(self.metadata, "sourceRoiX"),
            _metadata_int(self.metadata, "sourceRoiY"),
            _metadata_int(self.metadata, "sourceRoiWidth"),
            _metadata_int(self.metadata, "sourceRoiHeight"),
        )

    @property
    def module_index(self) -> int | None:
        value = self.metadata.get("moduleIndex")
        return None if value is None else int(value)

    @property
    def next_module_index(self) -> int | None:
        value = self.metadata.get("nextModuleIndex")
        return None if value is None else int(value)

    @property
    def start_module_index(self) -> int | None:
        value = self.metadata.get("startModuleIndex")
        return None if value is None else int(value)

    def write(self, image: object | None = None) -> None:
        from .shm import MONO8, MONO16, SHARED_FRAME_HEADER_SIZE, parse_frame_header

        try:
            import numpy as np
        except ImportError as exc:
            raise RuntimeError("numpy is required to write ScopeOne frames") from exc

        mapping_name = str(self.metadata["mappingName"])
        mapping_size = int(self.metadata["mappingSize"])
        with self._transport.open_frame(mapping_name, mapping_size, write=True) as view:
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
            self.image = np.array(contiguous, copy=True)


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
        self._transport = _make_transport(server_name)
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

    def save_frame_mapping(
        self,
        save_dir: str,
        base_name: str,
        format: str = "tiff",
        compression: bool = False,
        compression_level: int = 6,
        camera: str | None = None,
    ) -> list[str]:
        request = {
            "type": "save_frame_mapping",
            "saveDir": save_dir,
            "baseName": base_name,
            "format": format,
            "compression": bool(compression),
            "compressionLevel": int(compression_level),
        }
        if camera:
            request["camera"] = camera
        response = self._request(request)
        return list(response.get("paths", []))

    def close(self) -> None:
        self._transport.close()

    def _request(self, message: dict):
        payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
        if not payload or len(payload) > MAX_MESSAGE_BYTES:
            raise RuntimeError("ScopeOne control message is invalid or too large")

        framed = struct.pack("<I", len(payload)) + payload
        self._transport.send(framed)

        response_size = self._transport.recv_exact(4)
        payload_size = struct.unpack("<I", response_size)[0]
        if payload_size <= 0 or payload_size > MAX_MESSAGE_BYTES:
            raise RuntimeError("ScopeOne control response has invalid size")
        response = json.loads(self._transport.recv_exact(payload_size).decode("utf-8"))

        if not response.get("ok", False):
            error = response.get("error", "ScopeOne request failed")
            raise RuntimeError(error)
        return response

    def _frame_result_from_mapping_response(self, response: dict) -> FrameResult:
        from .shm import SHARED_FRAME_HEADER_SIZE, frame_to_ndarray, parse_frame_header

        mapping_name = str(response["mappingName"])
        mapping_size = int(response["mappingSize"])
        with self._transport.open_frame(mapping_name, mapping_size) as view:
            header = parse_frame_header(view[:SHARED_FRAME_HEADER_SIZE])
            image = frame_to_ndarray(header, view[SHARED_FRAME_HEADER_SIZE:])
        return FrameResult(image=image, metadata=dict(response), _transport=self._transport)


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
