"""Invert a live camera stream in Python and show it beside the raw image."""

import time

from scopeone import ScopeOne


def main() -> None:
    with ScopeOne() as scope:
        camera = scope.camera_ids()[0]
        layer_id = "python_stream"
        layer_key = f"static:{layer_id}"

        scope.start_preview(camera)
        scope.set_layer_layout("side_by_side")
        print(f"Streaming {camera}. Press Ctrl+C to stop.")

        last_frame = -1
        try:
            while True:
                frame = scope.latest_raw_frame(camera)
                if frame.frame_index == last_frame:
                    time.sleep(0.005)
                    continue
                last_frame = frame.frame_index

                max_val = (1 << frame.bits_per_sample) - 1
                scope.show_image(
                    max_val - frame.image,
                    layer_id=layer_id,
                    name="Python Processed",
                    camera=camera,
                    bits_per_sample=frame.bits_per_sample,
                )
        except KeyboardInterrupt:
            print("\nStopped.")
        finally:
            scope.remove_static_layer(layer_key)


if __name__ == "__main__":
    main()
