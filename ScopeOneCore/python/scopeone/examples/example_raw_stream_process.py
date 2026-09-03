"""Demonstrate real-time raw frame export, Python processing, and display reinjection."""

import time
import numpy as np

from scopeone import ScopeOne


def main() -> None:
    with ScopeOne() as scopeone:
        cameras = scopeone.camera_ids()
        if not cameras:
            raise RuntimeError(
                "No cameras available. Start ScopeOne and load a configuration first."
            )

        camera_id = cameras[0]
        print(f"Using camera: {camera_id}")

        # Ensure live preview is active
        scopeone.start_preview(camera_id)

        # Set display to side-by-side comparison mode
        scopeone.set_layer_layout("side_by_side")

        layer_id = "python_stream"
        layer_name = "Python Processed"
        layer_key = f"static:{layer_id}"

        last_frame_index = -1
        print("Streaming started. View ScopeOne side-by-side preview. Press Ctrl+C to stop...")

        try:
            while True:
                # 1. Pull latest raw frame
                frame = scopeone.latest_raw_frame(camera_id)

                # Avoid redundant computation on the same frame
                if frame.frame_index == last_frame_index:
                    time.sleep(0.005)
                    continue
                last_frame_index = frame.frame_index

                raw_img = frame.image

                # 2. Custom processing logic (example: negative / intensity inversion)
                max_val = (1 << frame.bits_per_sample) - 1
                processed_img = max_val - raw_img

                # 3. Reinject processed array into ScopeOne display
                scopeone.show_image(
                    processed_img,
                    layer_id=layer_id,
                    name=layer_name,
                    camera=camera_id,
                    bits_per_sample=frame.bits_per_sample,
                )
        except KeyboardInterrupt:
            print("\nStopping stream...")
        finally:
            try:
                scopeone.remove_static_layer(layer_key)
            except Exception:
                pass
            print("Cleaned up Python layer.")


if __name__ == "__main__":
    main()
