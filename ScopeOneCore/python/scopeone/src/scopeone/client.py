"""The ScopeOne client: control and frames of a running ScopeOne.

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
from collections import deque
from dataclasses import dataclass

from . import __version__
from ._shm import frame_to_ndarray, parse_frame_header, write_ndarray_to_frame

_IS_WINDOWS = os.name == "nt"

if _IS_WINDOWS:
    import pywintypes
    import win32file
    import win32pipe
else:
    import select
    import socket

# Windows: named pipe. Unix: QLocalServer turns this name into a socket file
# under the temp directory (e.g. /tmp/ScopeOne.Api.local).
WIN_LOCAL_SERVER_NAME = r"\\.\pipe\ScopeOne.Api.local"
UNIX_LOCAL_SERVER_NAME = "ScopeOne.Api.local"
LOCAL_SERVER_NAME = WIN_LOCAL_SERVER_NAME if _IS_WINDOWS else UNIX_LOCAL_SERVER_NAME

MAX_MESSAGE_BYTES = 64 * 1024 * 1024
_CONNECT_TIMEOUT_S = 5.0
# The hook requests of an experiment, which wait in ScopeOne until they are answered
_HOOK_EVENTS = ("before_event", "after_event")


class _WindowsPipeTransport:
    """Control channel + frame mapping over Windows named pipe / file mapping."""

    def __init__(self, server_name: str) -> None:
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

    def wait_readable(self, timeout: float | None) -> bool:
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            try:
                _, available, _ = win32pipe.PeekNamedPipe(self._handle, 0)
            except pywintypes.error as exc:
                raise RuntimeError(f"ScopeOne control connection failed: {exc}") from exc
            if available > 0:
                return True
            if deadline is not None and time.monotonic() >= deadline:
                return False
            time.sleep(0.01)

    def open_frame(self, mapping_name: str, mapping_size: int, write: bool = False):
        access = mmap.ACCESS_WRITE if write else mmap.ACCESS_READ
        return mmap.mmap(-1, mapping_size, tagname=mapping_name, access=access)

    def close(self) -> None:
        if self._handle is not None:
            win32file.CloseHandle(self._handle)
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

    def wait_readable(self, timeout: float | None) -> bool:
        readable, _, _ = select.select([self._sock], [], [], timeout)
        return bool(readable)

    def open_frame(self, mapping_name: str, mapping_size: int, write: bool = False):
        # The server publishes the frame as a POSIX shm object, which shm_open finds on Linux and
        # macOS alike
        import _posixshmem

        name = mapping_name if mapping_name.startswith("/") else f"/{mapping_name}"
        fd = _posixshmem.shm_open(name, os.O_RDWR if write else os.O_RDONLY, mode=0o600)
        try:
            prot = mmap.PROT_READ | (mmap.PROT_WRITE if write else 0)
            return mmap.mmap(fd, mapping_size, prot=prot)
        finally:
            os.close(fd)

    def close(self) -> None:
        if self._sock is not None:
            self._sock.close()
            self._sock = None


def _make_transport(server_name: str):
    if _IS_WINDOWS:
        return _WindowsPipeTransport(server_name)
    return _UnixSocketTransport(server_name)


@dataclass
class Frame:
    image: object
    metadata: dict

    @property
    def camera(self) -> str:
        return str(self.metadata.get("camera", ""))

    @property
    def width(self) -> int:
        return int(self.metadata["width"])

    @property
    def height(self) -> int:
        return int(self.metadata["height"])

    @property
    def pixel_format(self) -> str:
        return str(self.metadata.get("pixelFormat", ""))

    @property
    def bits_per_sample(self) -> int:
        return int(self.metadata["bitsPerSample"])

    @property
    def frame_index(self) -> int:
        return int(self.metadata["frameIndex"])

    @property
    def timestamp_ns(self) -> int:
        return int(self.metadata["timestampNs"])

    @property
    def source_roi(self) -> tuple[int, int, int, int] | None:
        if not bool(self.metadata["sourceRoiValid"]):
            return None
        return (
            int(self.metadata["sourceRoiX"]),
            int(self.metadata["sourceRoiY"]),
            int(self.metadata["sourceRoiWidth"]),
            int(self.metadata["sourceRoiHeight"]),
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


class ScopeOne:
    def __init__(self, endpoint: str = "local") -> None:
        self._transport = _make_transport(LOCAL_SERVER_NAME if endpoint == "local" else endpoint)
        self._next_request_id = 1
        # Event messages read while a response was awaited. Bounded, as a subscription that nobody
        # waits on, such as measurement during live processing, grew it without limit; the oldest
        # events go first.
        self._events = deque(maxlen=10000)
        # Hook requests read while a response or other events were awaited. Unbounded and apart from
        # the events, so that no other event pushes one out, as each holds an experiment until it is
        # answered or times out.
        self._hooks = deque()
        # The events this connection is subscribed to, as the last subscribe or unsubscribe returned
        self._subscribed = []
        # The shared frame mapping, opened on first use and kept for the connection, as mapping it
        # for every frame costs more than copying the frame
        self._mmap = None
        self._mapping = None
        app_version = str(self._request({"type": "ping"}).get("version", ""))
        # The Local API changes between minor versions, so a client of another MAJOR.MINOR is refused
        # here instead of failing on some later request
        if app_version.split(".")[:2] != __version__.split(".")[:2]:
            self._transport.close()
            series = ".".join(app_version.split(".")[:2])
            raise RuntimeError(f"scopeone {__version__} does not match ScopeOne {app_version}, "
                               f"install it with: pip install \"scopeone=={series}.*\"")

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()

    def load_config(self, config_path: str) -> dict:
        return self._request({"type": "load_config", "configPath": config_path})

    def unload_config(self) -> dict:
        return self._request({"type": "unload_config"})

    def version(self) -> str:
        response = self._request({"type": "version"})
        return str(response.get("version", ""))

    def status(self) -> dict:
        response = self._request({"type": "status"})
        return {
            "version": str(response.get("version", "")),
            "coreVersion": str(response.get("coreVersion", "")),
            "configurationState": str(response.get("configurationState", "unloaded")),
            "configurationError": str(response.get("configurationError", "")),
            "configurationOperationRunning": bool(
                response.get("configurationOperationRunning", False)
            ),
            "configurationPath": str(response.get("configurationPath", "")),
            "configurationComplete": bool(
                response.get("configurationComplete", False)
            ),
            "failedConfigurationDevices": list(
                response.get("failedConfigurationDevices", [])
            ),
            "cameraIds": list(response.get("cameraIds", [])),
            "loadedDevices": list(response.get("loadedDevices", [])),
            "runningPreviews": list(response.get("runningPreviews", [])),
            "processingBitDepth": int(response.get("processingBitDepth", 0)),
            "processingRealTime": bool(response.get("processingRealTime", False)),
            "processingModuleCount": int(response.get("processingModuleCount", 0)),
            "layers": list(response.get("layers", [])),
            "visibleLayers": list(response.get("visibleLayers", [])),
            "layerLayout": str(response.get("layerLayout", "")),
            "stageMosaic": dict(response.get("stageMosaic", {})),
            "recordingProgress": dict(response.get("recordingProgress", {})),
            "recordingWriter": dict(response.get("recordingWriter", {})),
        }

    def capabilities(self) -> dict:
        response = self._request({"type": "capabilities"})
        return dict(response.get("capabilities", {}))

    def subscribe(self, events: list[str] | None = None) -> list[str]:
        """Start sending the given events on this connection, or every event but "log" when events
        is empty. "log" comes only when it is named. Returns the subscribed events."""
        response = self._request({"type": "subscribe", "events": list(events or [])})
        self._subscribed = list(response.get("events", []))
        return list(self._subscribed)

    def unsubscribe(self, events: list[str] | None = None) -> list[str]:
        response = self._request({"type": "unsubscribe", "events": list(events or [])})
        self._subscribed = list(response.get("events", []))
        return list(self._subscribed)

    def wait_event(self, names: list[str] | None = None, timeout: float | None = None) -> dict | None:
        """Return the next event of the given names, or None after timeout seconds.

        Events of other names are dropped, except hook requests, which stay for a later wait.
        """
        deadline = None if timeout is None else time.monotonic() + timeout
        for hook in self._hooks:
            if not names or hook.get("event") in names:
                self._hooks.remove(hook)
                return hook
        while True:
            if self._events:
                event = self._events.popleft()
            else:
                remaining = None if deadline is None else max(0.0, deadline - time.monotonic())
                if not self._transport.wait_readable(remaining):
                    return None
                event = self._read_message()
                # Hook requests are kept apart, as dropping one left its event waiting for the
                # hook timeout
                if event.get("event") in _HOOK_EVENTS and names and event["event"] not in names:
                    self._hooks.append(event)
                    continue
            if not names or event.get("event") in names:
                return event

    def state_snapshot(self) -> dict:
        response = self._request({"type": "state_snapshot"})
        return dict(response.get("snapshot", {}))

    def camera_ids(self) -> list[str]:
        response = self._request({"type": "camera_ids"})
        return list(response.get("cameraIds", []))

    def loaded_devices(self) -> list[str]:
        response = self._request({"type": "loaded_devices"})
        return list(response.get("devices", []))

    def start_preview(self, camera: str = "All") -> bool:
        self._request({"type": "start_preview", "camera": camera})
        return True

    def stop_preview(self, camera: str = "All") -> bool:
        self._request({"type": "stop_preview", "camera": camera})
        return True

    def image_windows(self) -> dict:
        response = self._request({"type": "image_windows"})
        return {
            "activeDocumentId": str(response.get("activeDocumentId", "")),
            "documents": list(response.get("documents", [])),
        }

    def open_image_window(
        self,
        session_id: str,
        title: str | None = None,
        camera_id: str | None = None,
    ) -> dict:
        request = {
            "type": "open_image_window",
            "sessionId": session_id,
        }
        if title is not None:
            request["title"] = title
        if camera_id is not None:
            request["cameraId"] = camera_id
        response = self._request(request)
        return {
            "documentIds": list(response.get("documentIds", [])),
            "activeDocumentId": str(response.get("activeDocumentId", "")),
        }

    def activate_image_window(self, document_id: str) -> dict:
        response = self._request(
            {
                "type": "activate_image_window",
                "documentId": document_id,
            }
        )
        return dict(response["document"])

    def close_image_window(self, document_id: str | None = None) -> None:
        request = {"type": "close_image_window"}
        if document_id is not None:
            request["documentId"] = document_id
        self._request(request)

    def process_image_window(
        self,
        document_id: str | None = None,
        complete_stack: bool = False,
    ) -> dict:
        request = {
            "type": "process_image_window",
            "completeStack": bool(complete_stack),
        }
        if document_id is not None:
            request["documentId"] = document_id
        response = self._request(request)
        return dict(response["document"])

    def save_image_window(
        self,
        save_dir: str,
        base_name: str,
        document_id: str | None = None,
        format: str = "ome-tiff",
        compression: bool = False,
        compression_level: int = 6,
    ) -> dict:
        request = {
            "type": "save_image_window",
            "saveDir": save_dir,
            "baseName": base_name,
            "format": format,
            "compression": bool(compression),
            "compressionLevel": int(compression_level),
        }
        if document_id is not None:
            request["documentId"] = document_id
        response = self._request(request)
        return {
            "documentId": str(response["documentId"]),
            "message": str(response.get("message", "")),
        }

    def list_layers(self) -> list[dict]:
        response = self._request({"type": "list_layers"})
        return list(response.get("layers", []))

    def get_layer_histogram(self, layer_key: str) -> dict:
        response = self._request(
            {
                "type": "get_layer_histogram",
                "layerKey": layer_key,
            }
        )
        return dict(response["histogram"])

    def get_pixel_value(self, layer_key: str, x: int, y: int) -> int:
        response = self._request(
            {
                "type": "get_pixel_value",
                "layerKey": layer_key,
                "x": int(x),
                "y": int(y),
            }
        )
        return int(response["value"])

    def get_line_profile(
        self,
        layer_key: str,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
    ) -> list[int]:
        response = self._request(
            {
                "type": "get_line_profile",
                "layerKey": layer_key,
                "x1": int(x1),
                "y1": int(y1),
                "x2": int(x2),
                "y2": int(y2),
            }
        )
        return [int(value) for value in response.get("values", [])]

    def detect_particles(
        self,
        layer_key: str,
        threshold: int,
        min_area: int,
        max_area: int,
        max_particles: int = 1000,
        export_mask: bool = False,
        publish_mask: bool = False,
    ) -> dict:
        response = self._request(
            {
                "type": "detect_particles",
                "layerKey": layer_key,
                "threshold": int(threshold),
                "minArea": int(min_area),
                "maxArea": int(max_area),
                "maxParticles": int(max_particles),
                "exportMask": bool(export_mask),
                "publishMask": bool(publish_mask),
            }
        )
        result = {
            "layerKey": str(response.get("layerKey", "")),
            "threshold": int(response.get("threshold", 0)),
            "minArea": int(response.get("minArea", 0)),
            "maxArea": int(response.get("maxArea", 0)),
            "particleCount": int(response.get("particleCount", 0)),
            "truncated": bool(response.get("truncated", False)),
            "particles": list(response.get("particles", [])),
        }
        if "maskLayerKey" in response:
            result["maskLayerKey"] = str(response["maskLayerKey"])
        if "maskDocumentId" in response:
            result["maskDocumentId"] = str(response["maskDocumentId"])
        if "mask" in response:
            result["mask"] = self._frame_result_from_mapping_response(dict(response["mask"]))
        return result

    def layer_options(self) -> dict:
        response = self._request({"type": "layer_options"})
        return {
            "layouts": list(response.get("layouts", [])),
            "colormaps": list(response.get("colormaps", [])),
            "blendingModes": list(response.get("blendingModes", [])),
        }

    def set_layer_layout(self, layout: str) -> str:
        response = self._request(
            {
                "type": "set_layer_layout",
                "layout": layout,
            }
        )
        return str(response.get("layout", ""))

    def set_visible_layers(self, layer_keys: list[str]) -> list[str]:
        response = self._request(
            {
                "type": "set_visible_layers",
                "layerKeys": [str(layer_key) for layer_key in layer_keys],
            }
        )
        return list(response.get("visibleLayers", []))

    def set_layer_display(
        self,
        layer_key: str,
        visible: bool | None = None,
        opacity_percent: int | None = None,
        gamma: float | None = None,
        colormap: str | None = None,
        blending: str | None = None,
        levels: tuple[int, int, int] | None = None,
    ) -> dict:
        request = {
            "type": "set_layer_display",
            "layerKey": layer_key,
        }
        if visible is not None:
            request["visible"] = bool(visible)
        if opacity_percent is not None:
            request["opacityPercent"] = int(opacity_percent)
        if gamma is not None:
            request["gamma"] = float(gamma)
        if colormap is not None:
            request["colormap"] = colormap
        if blending is not None:
            request["blending"] = blending
        if levels is not None:
            min_level, max_level, max_possible = levels
            request["minLevel"] = int(min_level)
            request["maxLevel"] = int(max_level)
            request["maxPossible"] = int(max_possible)
        return dict(self._request(request))

    def auto_layer_levels(self, layer_key: str) -> dict:
        return dict(
            self._request(
                {
                    "type": "auto_layer_levels",
                    "layerKey": layer_key,
                }
            )
        )

    def full_layer_levels(self, layer_key: str) -> dict:
        return dict(
            self._request(
                {
                    "type": "full_layer_levels",
                    "layerKey": layer_key,
                }
            )
        )

    def set_layer_auto_stretch(self, layer_key: str, enabled: bool) -> dict:
        return dict(
            self._request(
                {
                    "type": "set_layer_auto_stretch",
                    "layerKey": layer_key,
                    "enabled": bool(enabled),
                }
            )
        )

    def get_source_display_transform(self, source_id: str) -> dict:
        return dict(
            self._request(
                {
                    "type": "get_source_display_transform",
                    "sourceId": source_id,
                }
            )
        )

    def set_source_display_transform(
        self,
        source_id: str,
        offset_x: int | None = None,
        offset_y: int | None = None,
        zoom_percent: int | None = None,
        flip_x: bool | None = None,
        flip_y: bool | None = None,
    ) -> dict:
        request = {
            "type": "set_source_display_transform",
            "sourceId": source_id,
        }
        if offset_x is not None:
            request["offsetX"] = int(offset_x)
        if offset_y is not None:
            request["offsetY"] = int(offset_y)
        if zoom_percent is not None:
            request["zoomPercent"] = int(zoom_percent)
        if flip_x is not None:
            request["flipX"] = bool(flip_x)
        if flip_y is not None:
            request["flipY"] = bool(flip_y)
        return dict(self._request(request))

    def reset_source_display_transform(self, source_id: str) -> dict:
        return dict(
            self._request(
                {
                    "type": "reset_source_display_transform",
                    "sourceId": source_id,
                }
            )
        )

    def move_layer(self, layer_key: str, offset: int) -> list[str]:
        response = self._request(
            {
                "type": "move_layer",
                "layerKey": layer_key,
                "offset": int(offset),
            }
        )
        return list(response.get("layers", []))

    def config_groups(self) -> list[str]:
        response = self._request({"type": "config_groups"})
        return list(response.get("groups", []))

    def configs(self, group: str) -> list[str]:
        response = self._request(
            {
                "type": "configs",
                "group": group,
            }
        )
        return list(response.get("configs", []))

    def current_config(self, group: str) -> str:
        response = self._request(
            {
                "type": "current_config",
                "group": group,
            }
        )
        return str(response.get("config", ""))

    def set_config(self, group: str, config: str) -> str:
        response = self._request(
            {
                "type": "set_config",
                "group": group,
                "config": config,
            }
        )
        return str(response.get("config", ""))

    def set_config_preset(self, group: str, preset: str, settings: list[dict]) -> None:
        """Create or replace preset of group in the loaded configuration. Each setting is a dict
        with "device", "property" and "value"."""
        self._request({"type": "set_config_preset", "group": group, "preset": preset,
                       "settings": list(settings)})

    def remove_config_preset(self, group: str, preset: str) -> None:
        self._request({"type": "remove_config_preset", "group": group, "preset": preset})

    def light_sources(self) -> list[dict]:
        response = self._request({"type": "light_sources"})
        return list(response.get("sources", []))

    def set_light_source(
        self,
        name: str,
        power_device: str,
        power_property: str,
        enable_device: str = "",
        enable_property: str = "",
        on_value: str = "",
        off_value: str = "",
        reading_device: str = "",
        reading_property: str = "",
    ) -> None:
        """Add or replace the light source name in the loaded configuration. The enable property
        switches it with on_value and off_value, the reading property reports the measured power."""
        self._request(
            {
                "type": "set_light_source",
                "name": name,
                "powerDevice": power_device,
                "powerProperty": power_property,
                "enableDevice": enable_device,
                "enableProperty": enable_property,
                "onValue": on_value,
                "offValue": off_value,
                "readingDevice": reading_device,
                "readingProperty": reading_property,
            }
        )

    def remove_static_layer(self, layer_key: str) -> None:
        self._request({"type": "remove_static_layer", "layerKey": layer_key})

    def clear_static_layers(self) -> None:
        self._request({"type": "clear_static_layers"})

    def create_line_markup(
        self,
        layer_key: str,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        label: str = "",
        role: str = "generic",
    ) -> str:
        response = self._request(
            {
                "type": "create_line_markup",
                "layerKey": layer_key,
                "x1": int(x1),
                "y1": int(y1),
                "x2": int(x2),
                "y2": int(y2),
                "label": label,
                "role": role,
            }
        )
        return str(response["markupId"])

    def create_rect_markup(
        self,
        layer_key: str,
        x: int,
        y: int,
        width: int,
        height: int,
        label: str = "",
        role: str = "generic",
    ) -> str:
        response = self._request(
            {
                "type": "create_rect_markup",
                "layerKey": layer_key,
                "x": int(x),
                "y": int(y),
                "width": int(width),
                "height": int(height),
                "label": label,
                "role": role,
            }
        )
        return str(response["markupId"])

    def list_markups(self, layer_key: str | None = None) -> list[dict]:
        request = {"type": "list_markups"}
        if layer_key:
            request["layerKey"] = layer_key
        response = self._request(request)
        return list(response.get("markups", []))

    def remove_markup(self, markup_id: str) -> None:
        self._request({"type": "remove_markup", "markupId": markup_id})

    def update_markup(
        self,
        markup_id: str,
        label: str | None = None,
        visible: bool | None = None,
        selected: bool | None = None,
        **geometry,
    ) -> dict:
        request = {
            "type": "update_markup",
            "markupId": markup_id,
        }
        if label is not None:
            request["label"] = label
        if visible is not None:
            request["visible"] = bool(visible)
        if selected is not None:
            request["selected"] = bool(selected)
        for key, value in geometry.items():
            if key not in {"x", "y", "width", "height", "x1", "y1", "x2", "y2"}:
                raise ValueError(f"Unsupported markup geometry field: {key}")
            request[key] = int(value)
        response = self._request(request)
        return dict(response["markup"])

    def clear_markups(self, layer_key: str | None = None) -> None:
        request = {"type": "clear_markups"}
        if layer_key:
            request["layerKey"] = layer_key
        self._request(request)

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

    def read_exposure(self, camera: str = "All") -> float:
        response = self._request(
            {
                "type": "read_exposure",
                "camera": camera,
            }
        )
        return float(response["exposureMs"])

    def set_exposure(self, exposure_ms: float, camera: str = "All") -> float | None:
        response = self._request(
            {
                "type": "set_exposure",
                "camera": camera,
                "exposureMs": float(exposure_ms),
            }
        )
        value = response.get("exposureMs")
        return None if value is None else float(value)

    def get_roi(self, camera: str) -> tuple[int, int, int, int]:
        response = self._request(
            {
                "type": "get_roi",
                "camera": camera,
            }
        )
        return (
            int(response["x"]),
            int(response["y"]),
            int(response["width"]),
            int(response["height"]),
        )

    def set_roi(
        self,
        camera: str,
        x: int,
        y: int,
        width: int,
        height: int,
    ) -> tuple[int, int, int, int]:
        response = self._request(
            {
                "type": "set_roi",
                "camera": camera,
                "x": int(x),
                "y": int(y),
                "width": int(width),
                "height": int(height),
            }
        )
        return (
            int(response["x"]),
            int(response["y"]),
            int(response["width"]),
            int(response["height"]),
        )

    def set_half_roi(self, camera: str) -> tuple[int, int, int, int]:
        response = self._request(
            {
                "type": "set_half_roi",
                "camera": camera,
            }
        )
        return (
            int(response["x"]),
            int(response["y"]),
            int(response["width"]),
            int(response["height"]),
        )

    def clear_roi(self, camera: str = "All") -> None:
        self._request(
            {
                "type": "clear_roi",
                "camera": camera,
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

    def start_stage_mosaic(
        self,
        camera_id: str,
        xy_stage_id: str,
        rows: int = 1,
        columns: int = 1,
        step_x_um: float = 0.0,
        step_y_um: float = 0.0,
        settle_ms: int = 150,
        return_to_start: bool = True,
        gallery_save_dir: str | None = None,
    ) -> dict:
        request = {
            "type": "start_stage_mosaic",
            "cameraId": camera_id,
            "xyStageId": xy_stage_id,
            "rows": int(rows),
            "columns": int(columns),
            "stepXUm": float(step_x_um),
            "stepYUm": float(step_y_um),
            "settleMs": int(settle_ms),
            "returnToStart": bool(return_to_start),
        }
        if gallery_save_dir is not None:
            request["gallerySaveDir"] = gallery_save_dir
        response = self._request(request)
        return dict(response.get("status", {}))

    def stage_mosaic_status(self) -> dict:
        response = self._request({"type": "stage_mosaic_status"})
        return dict(response.get("status", {}))

    def cancel_stage_mosaic(self) -> dict:
        response = self._request({"type": "cancel_stage_mosaic"})
        return dict(response.get("status", {}))

    def processing_state(self) -> dict:
        response = self._request({"type": "processing_modules"})
        return {
            "bitDepth": int(response.get("bitDepth", 0)),
            "realTime": bool(response.get("realTime", False)),
            "realTimeSource": str(response.get("realTimeSource", "")),
            "modules": list(response.get("modules", [])),
            "availableModules": list(response.get("availableModules", [])),
        }

    def processing_modules(self) -> list[dict]:
        return list(self.processing_state()["modules"])

    def set_processing_bit_depth(self, bit_depth: int) -> None:
        self._request(
            {
                "type": "set_processing_bit_depth",
                "bitDepth": int(bit_depth),
            }
        )

    def start_processing(self, camera_id: str | None = None) -> None:
        request = {"type": "set_realtime_processing", "enabled": True}
        if camera_id is not None:
            request["cameraId"] = str(camera_id)
        self._request(request)

    def stop_processing(self) -> None:
        self._request({"type": "set_realtime_processing", "enabled": False})

    def add_python_script(self, path: str) -> list[str]:
        """Read a script of Python processing modules, list its modules and remember the script.
        Returns the module ids, which add_processing_module then takes."""
        response = self._request({"type": "add_python_script", "path": str(path)})
        return list(response["moduleIds"])

    def remove_python_script(self, path: str) -> None:
        self._request({"type": "remove_python_script", "path": str(path)})

    def add_processing_module(
        self,
        kind: str,
        parameters: dict | None = None,
    ) -> int:
        request = {
            "type": "add_processing_module",
            "kind": kind,
        }
        if parameters is not None:
            request["parameters"] = dict(parameters)
        response = self._request(request)
        return int(response["index"])

    def remove_processing_module(self, index: int) -> None:
        self._request(
            {
                "type": "remove_processing_module",
                "index": int(index),
            }
        )

    def set_processing_module_parameters(self, index: int, parameters: dict) -> None:
        self._request(
            {
                "type": "set_processing_module_parameters",
                "index": int(index),
                "parameters": dict(parameters),
            }
        )

    def reset_processing_module_state(self, index: int) -> None:
        self._request(
            {
                "type": "reset_processing_module_state",
                "index": int(index),
            }
        )

    def triggers(self) -> list[dict]:
        response = self._request({"type": "triggers"})
        return list(response["triggers"])

    def set_triggers(self, triggers: list[dict]) -> list[dict]:
        response = self._request({"type": "set_triggers", "triggers": list(triggers)})
        return list(response["triggers"])

    def set_hooks(self, hooks: list[str], timeout_ms: int = 5000) -> dict:
        """Make the experiments started afterwards wait at "before_event" and/or
        "after_event" of every event for hook_reply. An empty list turns them off.
        The hooks belong to this connection."""
        response = self._request(
            {"type": "set_hooks", "hooks": list(hooks), "timeoutMs": int(timeout_ms)}
        )
        return {"hooks": list(response["hooks"]), "timeoutMs": int(response["timeoutMs"])}

    def hook_reply(self, hook_id: int, action: str = "continue", **changes) -> None:
        """Answer a hook with "continue", "skip" or "stop". Before an event, changes
        may set x, y, z and exposureMs of that event."""
        self._request({"type": "hook_reply", "hookId": int(hook_id), "action": action, **changes})

    def run_hooks(self, before=None, after=None, experiment_id: str | None = None) -> dict:
        """Answer the hooks until the experiment ends, and return its last experiment_state event.

        before(event) and after(data) get the event of the hook and the whole hook data. They
        return None or "continue", "skip", "stop", or a dict with "action" and changes such as
        {"z": 12.5}. Hooks without a function are answered with continue.
        """
        names = ["before_event", "after_event", "experiment_state"]
        # A subscription the caller had before stays, as unsubscribing it always ended the caller's
        # own waits for experiment_state
        subscribed = "experiment_state" in self._subscribed
        self.subscribe(["experiment_state"])
        try:
            # An experiment without events, or one that ended before the subscription, sends no
            # experiment_state that this loop could wait for, so the loop waited forever.
            if experiment_id is not None:
                status = self.experiment_status(experiment_id)
                if status["state"] != "Running":
                    return {"event": "experiment_state",
                            "data": {"experimentId": experiment_id, "state": status["state"]}}
            while True:
                message = self.wait_event(names)
                data = message.get("data", {})
                if message["event"] == "experiment_state":
                    if data.get("state") != "Running" and (
                        experiment_id is None or data.get("experimentId") == experiment_id
                    ):
                        return message
                    continue
                function = before if message["event"] == "before_event" else after
                if function is None:
                    reply = None
                elif message["event"] == "before_event":
                    reply = function(data["event"])
                else:
                    reply = function(data)
                if isinstance(reply, dict):
                    reply = dict(reply)
                    action = reply.pop("action", "continue")
                else:
                    action, reply = reply or "continue", {}
                # A hook that timed out or whose experiment was canceled while the function ran no
                # longer waits, and its refused reply ended the loop before the final experiment_state.
                try:
                    self.hook_reply(data["hookId"], action, **reply)
                except RuntimeError as exc:
                    if not str(exc).startswith("No event waits on hook"):
                        raise
        finally:
            if not subscribed:
                self.unsubscribe(["experiment_state"])

    def action_log(self, since: str | None = None) -> list[dict]:
        request = {"type": "action_log"}
        if since is not None:
            request["since"] = since
        return list(self._request(request)["entries"])

    def log(self, since: str | None = None, after: int | None = None) -> list[dict]:
        request = {"type": "log"}
        if since is not None:
            request["since"] = since
        if after is not None:
            request["after"] = after
        return list(self._request(request)["entries"])

    def experiment_document(self) -> dict:
        response = self._request({"type": "experiment_document"})
        return dict(response["document"])

    def validate_experiment(self, document: dict) -> dict:
        response = self._request(
            {
                "type": "validate_experiment",
                "document": dict(document),
            }
        )
        return dict(response["document"])

    def save_experiment(self, file_path: str, document: dict) -> str:
        response = self._request(
            {
                "type": "save_experiment",
                "filePath": file_path,
                "document": dict(document),
            }
        )
        return str(response["filePath"])

    def load_experiment(self, file_path: str) -> dict:
        response = self._request(
            {
                "type": "load_experiment",
                "filePath": file_path,
            }
        )
        return dict(response["document"])

    def start_experiment(self, document: dict):
        response = self._request(
            {
                "type": "start_experiment",
                "document": dict(document),
            }
        )
        return RecordingSession(self, str(response["experimentId"]))

    def experiment_status(self, experiment_id: str) -> dict:
        response = self._request(
            {
                "type": "experiment_status",
                "experimentId": experiment_id,
            }
        )
        return self._experiment_status_result(response)

    def cancel_experiment(self, experiment_id: str) -> dict:
        response = self._request(
            {
                "type": "cancel_experiment",
                "experimentId": experiment_id,
            }
        )
        return self._experiment_status_result(response)

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
        """Record frames and return the RecordingSession once the recording ends. ScopeOne refuses
        it on the connection that set hooks, as nothing would answer them while it waits; record
        on another connection, or use start_experiment and run_hooks."""
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
        return RecordingSession(self, str(response["sessionId"]))

    def _frame_mapping_info(self) -> dict:
        response = self._request({"type": "frame_mapping_info"})
        return {
            "mappingName": str(response["mappingName"]),
            "mappingSize": int(response["mappingSize"]),
            "headerBytes": int(response["headerBytes"]),
            "maxPayloadBytes": int(response["maxPayloadBytes"]),
            "pixelFormats": list(response.get("pixelFormats", [])),
        }

    def _write_frame_mapping(
        self,
        image: object,
        camera: str = "python",
        bits_per_sample: int | None = None,
        frame_index: int = 0,
        source_roi: tuple[int, int, int, int] | None = None,
    ) -> Frame:
        written, metadata = write_ndarray_to_frame(
            self._frame_mapping(),
            image,
            bits_per_sample=bits_per_sample,
            frame_index=frame_index,
            source_roi=source_roi,
        )
        metadata.update(
            {
                "camera": camera,
                "mappingName": self._mapping_name,
                "mappingSize": len(self._mapping),
            }
        )
        return Frame(image=written, metadata=metadata)

    def _process_frame_mapping(
        self,
        camera: str | None = None,
        start_module_index: int | None = None,
        end_module_index: int | None = None,
    ) -> Frame:
        request = {"type": "process_frame_mapping"}
        if camera:
            request["camera"] = camera
        _add_stage_fields(request, start_module_index, end_module_index)
        response = self._request(request)
        return self._frame_result_from_mapping_response(response)

    def process_image(
        self,
        image: object,
        camera: str = "python",
        bits_per_sample: int | None = None,
        start_module_index: int | None = None,
        end_module_index: int | None = None,
    ) -> Frame:
        frame = self._write_frame_mapping(image, camera, bits_per_sample)
        return self._process_frame_mapping(
            frame.camera,
            start_module_index,
            end_module_index,
        )

    def latest_raw_frame(self, camera: str) -> Frame:
        response = self._request(
            {
                "type": "latest_raw_frame",
                "camera": camera,
            }
        )
        return self._frame_result_from_mapping_response(response)

    def layer_frame(self, layer_key: str) -> Frame:
        response = self._request(
            {
                "type": "layer_frame",
                "layerKey": layer_key,
            }
        )
        return self._frame_result_from_mapping_response(response)

    def _show_frame_mapping(
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

    def show_image(
        self,
        image: object,
        layer_id: str = "python_result",
        name: str = "Python Result",
        camera: str = "python",
        bits_per_sample: int | None = None,
    ) -> str:
        frame = self._write_frame_mapping(image, camera, bits_per_sample)
        return self._show_frame_mapping(layer_id, name, frame.camera)

    def _save_frame_mapping(
        self,
        save_dir: str,
        base_name: str,
        format: str = "ome-tiff",
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

    def save_image(
        self,
        image: object,
        save_dir: str,
        base_name: str,
        format: str = "ome-tiff",
        compression: bool = False,
        compression_level: int = 6,
        camera: str = "python",
        bits_per_sample: int | None = None,
    ) -> list[str]:
        frame = self._write_frame_mapping(image, camera, bits_per_sample)
        return self._save_frame_mapping(
            save_dir,
            base_name,
            format,
            compression,
            compression_level,
            frame.camera,
        )

    def continue_pipeline(self, frame: Frame, image: object | None = None) -> Frame:
        if frame.next_module_index is None:
            raise ValueError("The frame has no next_module_index")
        self._write_frame(frame, image)
        return self._process_frame_mapping(
            frame.camera or None,
            start_module_index=frame.next_module_index,
        )

    def show_frame(
        self,
        frame: Frame,
        image: object | None = None,
        layer_id: str = "python_result",
        name: str = "Python Result",
    ):
        self._write_frame(frame, image)
        return self._show_frame_mapping(layer_id, name, frame.camera or None)

    def save_frame(
        self,
        frame: Frame,
        save_dir: str,
        base_name: str,
        image: object | None = None,
        format: str = "ome-tiff",
        compression: bool = False,
        compression_level: int = 6,
    ) -> list[str]:
        self._write_frame(frame, image)
        return self._save_frame_mapping(
            save_dir,
            base_name,
            format,
            compression,
            compression_level,
            frame.camera or None,
        )

    # Puts image, or the frame's own image, into the shared frame with the metadata of the frame
    def _write_frame(self, frame: Frame, image: object | None = None) -> None:
        frame.image, _ = write_ndarray_to_frame(
            self._frame_mapping(),
            frame.image if image is None else image,
            bits_per_sample=frame.bits_per_sample,
            frame_index=frame.frame_index,
            timestamp_ns=frame.timestamp_ns,
            source_roi=frame.source_roi,
        )

    def close(self) -> None:
        if self._mapping is not None:
            self._mapping.release()
            self._mmap.close()
            self._mapping = None
            self._mmap = None
        self._transport.close()

    def _frame_mapping(self) -> memoryview:
        if self._mapping is None:
            info = self._frame_mapping_info()
            self._mmap = self._transport.open_frame(info["mappingName"], info["mappingSize"], write=True)
            self._mapping = memoryview(self._mmap)
            self._mapping_name = info["mappingName"]
        return self._mapping

    def _request(self, message: dict):
        request = dict(message)
        request_id = self._next_request_id
        self._next_request_id += 1
        request["requestId"] = request_id
        payload = json.dumps(request, separators=(",", ":")).encode("utf-8")
        if not payload or len(payload) > MAX_MESSAGE_BYTES:
            raise RuntimeError("ScopeOne control message is invalid or too large")

        framed = struct.pack("<I", len(payload)) + payload
        self._transport.send(framed)

        response = self._read_message()
        while "event" in response and "requestId" not in response:
            (self._hooks if response["event"] in _HOOK_EVENTS else self._events).append(response)
            response = self._read_message()

        if response.get("requestId") != request_id:
            self._transport.close()
            raise RuntimeError("ScopeOne control response requestId mismatch")

        if not response.get("ok", False):
            error = response.get("error", "ScopeOne request failed")
            raise RuntimeError(error)
        return response

    def _read_message(self) -> dict:
        response_size = self._transport.recv_exact(4)
        payload_size = struct.unpack("<I", response_size)[0]
        if payload_size <= 0 or payload_size > MAX_MESSAGE_BYTES:
            self._transport.close()
            raise RuntimeError("ScopeOne control response has invalid size")
        return json.loads(self._transport.recv_exact(payload_size).decode("utf-8"))

    @staticmethod
    def _experiment_status_result(response: dict) -> dict:
        result = {
            "experimentId": str(response["experimentId"]),
            "state": str(response["state"]),
            "cancelRequested": bool(response.get("cancelRequested", False)),
            "document": dict(response["document"]),
        }
        if "sessionId" in response:
            result["sessionId"] = str(response["sessionId"])
            result["cameraIds"] = list(response.get("cameraIds", []))
            result["frameCount"] = int(response.get("frameCount", 0))
        if "progress" in response:
            result["progress"] = dict(response["progress"])
        if "writer" in response:
            result["writer"] = dict(response["writer"])
        return result

    def _frame_result_from_mapping_response(self, response: dict) -> Frame:
        mapping = self._frame_mapping()
        image = frame_to_ndarray(parse_frame_header(mapping), mapping)
        return Frame(image=image, metadata=dict(response))


# An experiment, which record also starts, and the frames it recorded in memory. Core keeps both
# under the experiment id.
class RecordingSession:
    def __init__(self, client: ScopeOne, session_id: str) -> None:
        self._client = client
        self._session_id = session_id

    def experiment_id(self) -> str:
        return self._session_id

    def status(self) -> dict:
        return self._client.experiment_status(self._session_id)

    def document(self) -> dict:
        return dict(self.status()["document"])

    def cancel(self) -> dict:
        return self._client.cancel_experiment(self._session_id)

    # An experiment that streamed to disk has no frames in memory to release
    def close(self) -> None:
        if self._session_id and "sessionId" in self.status():
            self._client._request(
                {
                    "type": "session_close",
                    "sessionId": self._session_id,
                }
            )
        self._session_id = ""

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()

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

    def frame(self, camera: str, index: int) -> Frame:
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
    ) -> Frame:
        request = {
            "type": "session_process_frame",
            "sessionId": self._session_id,
            "camera": camera,
            "index": index,
        }
        _add_stage_fields(request, start_module_index, end_module_index)
        response = self._client._request(request)
        return self._client._frame_result_from_mapping_response(response)

    def frames(self, camera: str) -> list[Frame]:
        return [self.frame(camera, index) for index in range(self.frame_count(camera))]

    def save(
        self,
        save_dir: str,
        base_name: str,
        format: str = "ome-tiff",
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
