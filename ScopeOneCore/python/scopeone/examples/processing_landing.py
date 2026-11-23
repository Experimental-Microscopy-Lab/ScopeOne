# Finds a particle landing in the DRA image of the iSCAT Processing module. Copy it into
# processing/python/ and put "Landing" after iSCAT Processing in DRA mode with 32 frames. Each
# frame reports the deepest dark spot as the measurement "landing", its contrast as a positive
# number, so a trigger can act on it:
#
#     scope.set_triggers([{"measurement": "landing", "compare": "above", "threshold": 0.008,
#                          "action": "record", "frames": 200, "cooldownMs": 2000}])
import numpy as np

from scopeone.processing import Real, module


@module("example.python.landing", "Landing",
        Real("gain", "Contrast gain", 20.0, 1.0, 500.0, 1.0, 1),
        Real("sigma", "Sigma", 1.7, 0.5, 20.0, 0.1, 1))
def landing(image, gain, sigma, bits):
    # While DRA fills its two windows the iSCAT module passes the camera frames on, which read as a
    # deep dark spot; only its 16-bit contrast images are measured
    if bits != 16:
        return image, []
    # The iSCAT module writes the contrast centered at 32768 and scaled by its contrast gain
    contrast = (image.astype(np.float32) - 32768.0) / (gain * 32768.0)
    # A Gaussian of half the width of the spot averages the noise of the pixels; a wider one also
    # takes in the bright ring and cancels the dark center
    fy = np.fft.fftfreq(contrast.shape[0])[:, None]
    fx = np.fft.rfftfreq(contrast.shape[1])[None, :]
    kernel = np.exp(-2.0 * (np.pi * sigma) ** 2 * (fx * fx + fy * fy))
    smooth = np.fft.irfft2(np.fft.rfft2(contrast) * kernel, s=contrast.shape)
    y, x = np.unravel_index(np.argmin(smooth), smooth.shape)
    return image, [{"name": "landing", "value": float(-smooth[y, x]), "x": int(x), "y": int(y)}]
