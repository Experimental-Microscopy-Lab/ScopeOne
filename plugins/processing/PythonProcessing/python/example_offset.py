# A processing module in Python. ScopeOne lists it in the pipeline as "Python Offset".
import numpy as np

from scopeone.processing import Integer, module


@module("example.python.offset", "Python Offset", Integer("offset", "Offset", 10, 0, 65535))
def offset(image, offset):
    top = np.iinfo(image.dtype).max
    return np.clip(image.astype(np.int64) + offset, 0, top).astype(image.dtype)
