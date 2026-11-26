"""Processing modules written in Python for the ScopeOne pipeline.

A module is a function that takes a 2D numpy array (uint8 or uint16) and its parameters and
returns a 2D uint8 or uint16 array. A function with a ``bits`` argument also gets the bits per
sample of the frame, 12 for a 12-bit camera in a uint16 array. Add the script with Python > Add
Script... in the pipeline panel, or with ``ScopeOne.add_python_script``, and its modules show up in
the pipeline beside the C++ modules, as "Offset (Python)" for the one below::

    import numpy as np
    from scopeone.processing import module, Integer

    @module("example.py.offset", "Offset", Integer("offset", "Offset", 10, 0, 1000))
    def offset(image, offset, bits):
        return np.clip(image.astype(np.int32) + offset, 0, (1 << bits) - 1).astype(image.dtype)

A module can also report what it measured on the frame by returning ``(array, measurements)``,
where measurements is a list of dicts with ``name``, ``value`` and optional ``x`` and ``y`` in image
pixels. ScopeOne sends them as the ``measurement`` event of the Local API, and the triggers of Core
act on them::

    @module("example.py.peak", "Peak")
    def peak(image):
        y, x = np.unravel_index(np.argmax(image), image.shape)
        return image, [{"name": "peak", "value": float(image[y, x]), "x": int(x), "y": int(y)}]

ScopeOne runs the script in its own Python process (``python -m scopeone.processing serve
script.py``), so the interpreter that runs it needs numpy and this package.
"""

from __future__ import annotations

import importlib.util
import inspect
import json
import mmap
import os
import sys
from dataclasses import dataclass, field
from typing import Any, Callable

import numpy as np


@dataclass
class Parameter:
    key: str
    name: str
    default: Any
    type: str = "int"
    minimum: Any = None
    maximum: Any = None
    step: Any = None
    decimals: int = 0
    choices: list = field(default_factory=list)

    def describe(self) -> dict:
        description = {"key": self.key, "name": self.name, "type": self.type, "default": self.default}
        for key, value in (("minimum", self.minimum), ("maximum", self.maximum), ("step", self.step)):
            if value is not None:
                description[key] = value
        if self.decimals:
            description["decimals"] = self.decimals
        if self.choices:
            description["choices"] = [{"name": name, "value": value} for name, value in self.choices]
        return description


def Integer(key, name, default, minimum=None, maximum=None, step=1) -> Parameter:
    return Parameter(key, name, int(default), "int", minimum, maximum, step)


def Real(key, name, default, minimum=None, maximum=None, step=None, decimals=2) -> Parameter:
    return Parameter(key, name, float(default), "real", minimum, maximum, step, decimals)


def Boolean(key, name, default=False) -> Parameter:
    return Parameter(key, name, bool(default), "bool")


def Choice(key, name, default, choices) -> Parameter:
    """choices is a list of (name, value) pairs"""
    return Parameter(key, name, default, "choice", choices=list(choices))


@dataclass
class _Module:
    id: str
    name: str
    parameters: tuple
    function: Callable
    takes_bits: bool


_modules: dict[str, _Module] = {}


def module(id: str, name: str, *parameters: Parameter):
    """Registers the decorated function as a processing module"""

    def register(function):
        takes_bits = "bits" in inspect.signature(function).parameters
        _modules[id] = _Module(id, name, parameters, function, takes_bits)
        return function

    return register


def _load(path: str) -> None:
    spec = importlib.util.spec_from_file_location("scopeone_user_module", path)
    script = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(script)


def _describe() -> None:
    modules = [{"id": m.id, "name": m.name, "parameters": [p.describe() for p in m.parameters]}
               for m in _modules.values()]
    sys.stdout.write(json.dumps(modules) + "\n")
    sys.stdout.flush()


# The shared memory Core creates for the frames of a module
def _open_frames(name: str, size: int) -> mmap.mmap:
    if sys.platform == "win32":
        return mmap.mmap(-1, size, tagname=name)
    import _posixshmem

    fd = _posixshmem.shm_open(name, os.O_RDWR, mode=0o600)
    try:
        return mmap.mmap(fd, size)
    finally:
        os.close(fd)


def _send(stream, header: dict) -> None:
    # NaN in a measurement would write a header Core cannot parse, so it is refused here and the
    # module reports an error instead
    stream.write(json.dumps(header, allow_nan=False).encode() + b"\n")
    stream.flush()


# One frame per request: a JSON line with the frame in the shared memory it names, and a JSON line
# back with the result in the same memory
def _serve() -> None:
    requests = sys.stdin.buffer
    replies = sys.stdout.buffer
    # Anything the module prints goes to stderr, so it cannot break the frame stream
    sys.stdout = sys.stderr
    name = None
    while True:
        line = requests.readline()
        if not line:
            return
        request = json.loads(line)
        if request["mapping"] != name:
            name = request["mapping"]
            frames = _open_frames(name, request["capacity"])
        try:
            dtype = np.uint16 if request["format"] == "mono16" else np.uint8
            rows = np.ndarray((request["height"], request["stride"] // dtype().itemsize), dtype, frames)
            # A copy, as a module may keep the frame and the next frame overwrites the memory
            image = rows[:, : request["width"]].copy()
            selected = _modules[request["module"]]
            arguments = dict(request["parameters"])
            if selected.takes_bits:
                arguments["bits"] = request["bits"]
            result = selected.function(image, **arguments)
            measurements = []
            if isinstance(result, tuple):
                result, measurements = result
            result = np.asarray(result)
            if result.ndim != 2 or result.dtype not in (np.uint8, np.uint16):
                raise TypeError("a module must return a 2D uint8 or uint16 array")
            # The memory only grows, so it is not the limit itself, which then depended on earlier frames
            if result.nbytes > 2 * request["height"] * request["stride"]:
                raise ValueError("a module must return at most twice the bytes of the frame")
            np.ndarray(result.shape, result.dtype, frames)[...] = result
            header = {"width": result.shape[1], "height": result.shape[0],
                      "format": "mono16" if result.dtype == np.uint16 else "mono8",
                      "bytes": result.nbytes}
            if measurements:
                header["measurements"] = list(measurements)
            _send(replies, header)
        except Exception as error:
            _send(replies, {"error": f"{type(error).__name__}: {error}"})


def main(argv: list[str]) -> None:
    command, path = argv[1], argv[2]
    _load(path)
    if command == "describe":
        _describe()
    else:
        _serve()


if __name__ == "__main__":
    # Run inside the imported scopeone.processing, where the scripts register their modules,
    # not in this __main__ copy of it
    from scopeone.processing import main as _main

    _main(sys.argv)
