"""Processing modules written in Python for the ScopeOne pipeline.

A module is a function that takes a 2D numpy array (uint8 or uint16) and its parameters and
returns a 2D uint8 or uint16 array. Put the script in the ``python`` folder of a ScopeOne
processing plugin directory and it shows up in the pipeline beside the C++ modules::

    import numpy as np
    from scopeone.processing import module, Integer

    @module("example.py.offset", "Python Offset", Integer("offset", "Offset", 10, 0, 1000))
    def offset(image, offset):
        return np.clip(image.astype(np.int32) + offset, 0, np.iinfo(image.dtype).max).astype(image.dtype)

ScopeOne runs the script in its own Python process (``python -m scopeone.processing serve
script.py``), so the interpreter that runs it needs numpy and this package.
"""

from __future__ import annotations

import importlib.util
import json
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


_modules: dict[str, _Module] = {}


def module(id: str, name: str, *parameters: Parameter):
    """Registers the decorated function as a processing module"""

    def register(function):
        _modules[id] = _Module(id, name, parameters, function)
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


def _read_exactly(stream, count: int) -> bytes:
    data = bytearray()
    while len(data) < count:
        chunk = stream.read(count - len(data))
        if not chunk:
            raise EOFError
        data.extend(chunk)
    return bytes(data)


def _send(stream, header: dict, payload: bytes = b"") -> None:
    stream.write(json.dumps(header).encode() + b"\n")
    stream.write(payload)
    stream.flush()


# One frame per request: a JSON header line, then the pixel bytes, and the same back
def _serve() -> None:
    requests = sys.stdin.buffer
    replies = sys.stdout.buffer
    # Anything the module prints goes to stderr, so it cannot break the frame stream
    sys.stdout = sys.stderr
    while True:
        line = requests.readline()
        if not line:
            return
        request = json.loads(line)
        payload = _read_exactly(requests, request["bytes"])
        try:
            dtype = np.uint16 if request["format"] == "mono16" else np.uint8
            rows = np.frombuffer(payload, dtype=dtype).reshape(request["height"], request["stride"] // dtype().itemsize)
            image = rows[:, : request["width"]]
            selected = _modules[request["module"]]
            result = np.ascontiguousarray(selected.function(image, **request["parameters"]))
            if result.ndim != 2 or result.dtype not in (np.uint8, np.uint16):
                raise TypeError("a module must return a 2D uint8 or uint16 array")
            _send(replies, {"width": result.shape[1], "height": result.shape[0],
                            "format": "mono16" if result.dtype == np.uint16 else "mono8",
                            "bytes": result.nbytes}, result.tobytes())
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
