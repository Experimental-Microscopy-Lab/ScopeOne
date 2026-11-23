"""Step into a running experiment at every event, as the acquisition hooks of pycro-manager do.

The plan images three positions in two channels. The before hook skips the second position and
moves the focus of the third, and the after hook prints the mean of every image.
"""

from pathlib import Path

from scopeone import ScopeOne


CONFIG_PATH = str(Path(__file__).resolve().parents[4] / "config" / "MMConfig_demo.cfg")


def main() -> None:
    with ScopeOne() as scope:
        scope.load_config(CONFIG_PATH)

        document = scope.experiment_document()
        document["plan"].update({
            "positions": [{"x": 0.0, "y": 0.0}, {"x": 100.0, "y": 0.0}, {"x": 200.0, "y": 0.0}],
            "order": ["Time", "Z", "XY", "Channel"],
            "channelGroup": "Channel",
            "channels": [
                {"preset": "DAPI", "exposureMs": 10.0, "zOffsetUm": 0.0, "zStack": True, "skipFrames": 0},
                {"preset": "FITC", "exposureMs": 20.0, "zOffsetUm": 0.0, "zStack": True, "skipFrames": 0},
            ],
        })
        z0 = scope.read_z_position()

        def before(event):
            if event["positionIndex"] == 1:
                return "skip"
            if event["positionIndex"] == 2:
                return {"z": z0 + 2.0}
            return None

        def after(data):
            event = data["event"]
            frame = scope.latest_raw_frame(data["frames"][0]["cameraId"])
            print(f"position {event['positionIndex']} {event['channelPreset']}: mean {frame.image.mean():.1f}")

        scope.set_hooks(["before_event", "after_event"])
        try:
            session = scope.start_experiment(document)
            final = scope.run_hooks(before, after, session.experiment_id())
            print("Experiment:", final["data"]["state"])
            session.close()
        finally:
            scope.set_hooks([])
            scope.unload_config()


if __name__ == "__main__":
    main()
