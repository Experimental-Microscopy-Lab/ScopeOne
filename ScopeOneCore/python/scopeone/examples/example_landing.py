"""Record every particle landing and save it, with the detection in the pipeline.

Start ScopeOne and load a configuration with the example camera first. The script shows the
camera its iSCAT Landing scene, adds processing_landing.py beside it and builds the pipeline:
iSCAT Processing in DRA mode, then the Landing module of that script, which measures
every frame. A Core trigger records 200 frames when a landing crosses the threshold, starting with
the frames from before it. The script only waits for the recordings and saves each one.
"""

from pathlib import Path

from scopeone import RecordingSession, ScopeOne


GAIN = 20.0
THRESHOLD = 0.008
SAVE_DIR = str(Path.home())


# The trigger records into memory, so a completed recording is saved, and every recording is then
# closed to free that memory
def finish(data):
    with RecordingSession(scope, data["experimentId"]) as session:
        if data["state"] == "Completed":
            print("Saved:", session.save(SAVE_DIR, f"landing_{data['experimentId'][:8]}"))
        else:
            print("Recording", data["state"])


scope = ScopeOne()
camera = scope.camera_ids()[0]
scope.set_property(camera, "Pattern", "iSCAT Landing")
scope.add_python_script(str(Path(__file__).with_name("processing_landing.py")))
iscat = scope.add_processing_module("iscat.processing", {"mode": 2, "frames": 32, "contrast_gain": GAIN})
landing = scope.add_processing_module("example.python.landing", {"gain": GAIN})
scope.set_triggers([{"measurement": "landing", "camera": camera, "compare": "above",
                     "threshold": THRESHOLD, "action": "record", "frames": 200,
                     "preTriggerFrames": 50, "cooldownMs": 2000}])
scope.subscribe(["trigger_fired", "experiment_state"])
scope.start_preview(camera)
scope.start_processing()
print("Waiting for landings. Press Ctrl+C to stop.")

recording = None
try:
    while True:
        message = scope.wait_event(["trigger_fired", "experiment_state"])
        data = message["data"]
        if message["event"] == "trigger_fired":
            measurement = data["measurement"]
            print(f"Landing at ({measurement['x']}, {measurement['y']}), "
                  f"contrast {measurement['value']:.4f}")
        elif data["state"] == "Running":
            recording = data["experimentId"]
        else:
            # A recording that failed or was canceled ends too, and was otherwise waited for on exit
            recording = None
            finish(data)
except KeyboardInterrupt:
    print("\nStopped.")
finally:
    scope.set_triggers([])
    # A recording that runs holds the camera, which then refuses to stop its preview, so it
    # is finished and saved first
    while recording:
        data = scope.wait_event(["experiment_state"])["data"]
        if data["experimentId"] == recording and data["state"] != "Running":
            finish(data)
            recording = None
    scope.stop_processing()
    scope.remove_processing_module(landing)
    scope.remove_processing_module(iscat)
    scope.stop_preview(camera)
