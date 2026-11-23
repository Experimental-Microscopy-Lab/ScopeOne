"""Record when a particle lands, with the detection in a plain script.

Load a configuration with the example camera first. The script shows the camera its iSCAT
Landing scene, runs iSCAT Processing in DRA mode on the live frames, looks at the DRA image for
a new dark spot and records 200 frames when one appears. processing_landing.py does the same
detection inside the pipeline, where a Core trigger acts on every frame.
"""

import time
from pathlib import Path

import numpy as np

from scopeone import ScopeOne


GAIN = 20.0
SIGMA = 1.7
THRESHOLD = 0.008
SAVE_DIR = str(Path.home())


def find_landing(image):
    """The contrast of the deepest dark spot of a DRA image, as a positive number, and where it is."""
    contrast = (image.astype(np.float32) - 32768.0) / (GAIN * 32768.0)
    fy = np.fft.fftfreq(contrast.shape[0])[:, None]
    fx = np.fft.rfftfreq(contrast.shape[1])[None, :]
    kernel = np.exp(-2.0 * (np.pi * SIGMA) ** 2 * (fx * fx + fy * fy))
    smooth = np.fft.irfft2(np.fft.rfft2(contrast) * kernel, s=contrast.shape)
    y, x = np.unravel_index(np.argmin(smooth), smooth.shape)
    return float(-smooth[y, x]), int(x), int(y)


def main() -> None:
    with ScopeOne() as scope:
        camera = scope.camera_ids()[0]
        scope.set_property(camera, "Pattern", "iSCAT Landing")
        module = scope.add_processing_module("iscat.processing", {"mode": 2, "frames": 32, "contrast_gain": GAIN})
        scope.start_preview(camera)
        scope.start_processing()
        # The processed layer has no frame until the pipeline has run once
        time.sleep(1.0)
        print("Waiting for landings. Press Ctrl+C to stop.")

        last_frame = -1
        try:
            while True:
                frame = scope.layer_frame(f"proc:{camera}")
                if frame.frame_index == last_frame:
                    time.sleep(0.01)
                    continue
                last_frame = frame.frame_index
                # While DRA fills its two windows the iSCAT module passes the camera frames on, which
                # read as a deep dark spot
                if frame.bits_per_sample != 16:
                    continue
                depth, x, y = find_landing(frame.image)
                # The recording takes longer than a landing stays in the DRA image, so the same
                # landing is not found again after it
                if depth > THRESHOLD:
                    print(f"Landing at ({x}, {y}), contrast {depth:.4f}, frame {frame.frame_index}")
                    with scope.record(frames=200, camera=camera) as session:
                        print("Saved:", session.save(SAVE_DIR, f"landing_{frame.frame_index}", format="ome-tiff"))
        except KeyboardInterrupt:
            print("\nStopped.")
        finally:
            scope.stop_processing()
            scope.remove_processing_module(module)
            scope.stop_preview(camera)


if __name__ == "__main__":
    main()
