# A bandpass filter on the FFT of the frame. Add this file with Python > Add Script... in the
# pipeline panel and ScopeOne lists it as "Frequency Filter (Python)". The script only builds the
# mask; the FFT runs in the C++ of pocketfft on every core (scipy.fft), and the frame comes and goes
# through the shared memory of ScopeOne. Needs scipy.
from functools import lru_cache

import numpy as np
from scipy import fft

from scopeone.processing import Choice, Real, module


# The weight of the bandpass on the half spectrum of the real FFT, built once per size and setting
@lru_cache(maxsize=4)
def mask(shape, min_size, max_size, kind):
    fy = fft.fftfreq(shape[0])[:, None]
    fx = fft.rfftfreq(shape[1])[None, :]
    r2 = (2 * np.pi) ** 2 * (fx * fx + fy * fy)
    if kind == 1:
        return ((r2 * max_size**2 > 1) & (r2 * min_size**2 < 1)).astype(np.float32)
    return (np.exp(-r2 * min_size**2 / 2) - np.exp(-r2 * max_size**2 / 2)).astype(np.float32)


@module("example.python.frequency_filter", "Frequency Filter",
        Real("min_feature_size", "Min feature size", 2.0, 0.0, 1000.0, 0.1),
        Real("max_feature_size", "Max feature size", 10.0, 0.0, 1000.0, 0.1),
        Choice("filter_kind", "Filter kind", 0, [("Smooth", 0), ("Hard", 1)]))
def frequency_filter(image, min_feature_size, max_feature_size, filter_kind, bits):
    shape = tuple(fft.next_fast_len(n, real=True) for n in image.shape)
    spectrum = fft.rfft2(image.astype(np.float32), s=shape, workers=-1)
    spectrum *= mask(shape, min_feature_size, max_feature_size, filter_kind)
    filtered = fft.irfft2(spectrum, s=shape, workers=-1)[: image.shape[0], : image.shape[1]]
    # Stretched to the full range of the frame, as the built-in filter does
    low, high = filtered.min(), filtered.max()
    filtered -= low
    filtered *= ((1 << bits) - 1) / max(high - low, np.finfo(np.float32).tiny)
    return filtered.astype(image.dtype)
