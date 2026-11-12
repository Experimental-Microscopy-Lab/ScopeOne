# A processing module in Python. Copy it into processing/python/ and ScopeOne lists it
# in the pipeline as "Python Offset".
import numpy as np

from scopeone.processing import Integer, module


@module("example.python.offset", "Python Offset", Integer("offset", "Offset", 10, 0, 65535))
def offset(image, offset, bits):
    return np.clip(image.astype(np.int64) + offset, 0, (1 << bits) - 1).astype(image.dtype)
