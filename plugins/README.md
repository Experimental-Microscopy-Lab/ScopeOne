# Extending ScopeOne

ScopeOne has two public extension points. Both run outside the ScopeOne process, so a crash in an
extension does not take ScopeOne down.

| Extension | You write | Runs in | Example |
|---|---|---|---|
| Driver | a `Driver` in C++ against `scopeone::DriverSDK` | `ScopeOne_DriverHost`, one process per device | `drivers/ExampleCamera`, `drivers/ExamplePmt` |
| Python processing module | a decorated Python function | its own Python process | `ScopeOneCore/python/scopeone/examples/processing_offset.py` |

Drivers have their own folder and guide, [drivers/README.md](../drivers/README.md). This folder
builds the drivers together with the CUDA and ONNX processing modules in `ScopeOneCuda` and
`ScopeOneInference`. Those two are built-in plugins, built and released with ScopeOne, because
they link CUDA and ONNX Runtime. Their interface, `scopeone/ProcessingPlugin.h`, changes with
ScopeOne and is not meant for code outside this repository, and ScopeOne loads native
processing plugins only from `plugins/processing` beside the executable, never from the user's
directory. Everything else, including the tool
windows and the PTU file reader, is part of ScopeOne itself.

## Python processing modules

```python
import numpy as np
from scopeone.processing import Integer, Real, Boolean, Choice, module

@module("my.python.threshold", "My Threshold", Integer("level", "Level", 100, 0, 65535))
def threshold(image, level, bits):
    return np.where(image > level, (1 << bits) - 1, 0).astype(image.dtype)
```

The function gets a 2D `uint8` or `uint16` array and the parameters by name, and returns a 2D
`uint8` or `uint16` array. A function with a `bits` argument also gets the bits per sample of
the frame, 12 for a 12-bit camera in a `uint16` array, so `bits` is not a parameter key. Put
the script in `plugins/processing/python/` beside ScopeOne or in the same folder of the user's
plugin directory. The modules appear in the pipeline after a restart. ScopeOne ships no script
there; `ScopeOneCore/python/scopeone/examples/processing_offset.py` is one to copy.

Each module in a pipeline runs in its own Python process (`python -m scopeone.processing
serve script.py`). That interpreter needs numpy and the `scopeone` package
(`pip install ScopeOneCore/python/scopeone`). Choose it under **Settings > Python
Interpreter**, or with `SCOPEONE_PYTHON`, which takes precedence; otherwise ScopeOne runs
`python3` (`python` on Windows). Output printed by a module goes to the log, and an exception
in it shows as the error of that pipeline step.

ScopeOne first runs `python -m scopeone.processing describe script.py`, which prints one JSON
line with the modules of the script and their parameters. While the pipeline runs, each frame is
one request on stdin: a JSON line with `module`, `parameters`, `width`, `height`, `stride`,
`format` (`mono8` or `mono16`), `bits` and `bytes`, followed by that many pixel bytes. The reply
on stdout has the same form with `width`, `height`, `format` and `bytes`, or a JSON line with
`error`.
