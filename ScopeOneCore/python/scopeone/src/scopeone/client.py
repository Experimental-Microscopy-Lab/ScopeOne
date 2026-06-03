"""Backend clients for the public ScopeOne facade."""

from __future__ import annotations

import json
import mmap
import struct
import time

try:
    import pywintypes
    import win32file
except ImportError:
    pywintypes = None
    win32file = None

LOCAL_SERVER_NAME = r"\\.\pipe\ScopeOne.Api.local"
MAX_MESSAGE_BYTES = 256 * 1024

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

    def record(
        self,
        frames: int,
        camera: str = "All",
        timeout_ms: int = 120000,
    ):
        response = self._request(
            {
                "type": "record",
                "frames": frames,
                "camera": camera,
                "timeoutMs": timeout_ms,
            }
        )
        return ExternalRecordingSession(self, str(response["sessionId"]))

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

    def frame(self, camera: str, index: int):
        from .shm import SHARED_FRAME_HEADER_SIZE, frame_to_ndarray, parse_frame_header

        response = self._client._request(
            {
                "type": "session_frame",
                "sessionId": self._session_id,
                "camera": camera,
                "index": index,
            }
        )
        mapping_name = str(response["mappingName"])
        mapping_size = int(response["mappingSize"])
        with mmap.mmap(-1, mapping_size, tagname=mapping_name, access=mmap.ACCESS_READ) as view:
            header = parse_frame_header(view[:SHARED_FRAME_HEADER_SIZE])
            return frame_to_ndarray(header, view[SHARED_FRAME_HEADER_SIZE:])

    def frames(self, camera: str):
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
