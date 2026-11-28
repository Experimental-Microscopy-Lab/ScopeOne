# A processing module in Python. Add this file with Python > Add Script... in the pipeline panel
# and ScopeOne lists it as "Offset (Python)".
import numpy as np

from scopeone.processing import Integer, module


@module("example.python.offset", "Offset", Integer("offset", "Offset", 10, 0, 65535))
def offset(image, offset, bits):
    return np.clip(image.astype(np.int64) + offset, 0, (1 << bits) - 1).astype(image.dtype)
