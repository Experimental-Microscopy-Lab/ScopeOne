"""CPU and GPU use of ScopeOne in five operating states.

Install the client and psutil once (nvidia-smi comes with the NVIDIA driver)
    python -m pip install ScopeOneCore/python/scopeone psutil
Load the configuration in ScopeOne first, then run
    python scripts/resource_use.py --camera "HamamatsuHam_DCAM" --out resource_use.csv
The same command works in PowerShell on Windows and in a Linux shell, and the processes
ScopeOne(.exe) and ScopeOne_DriverHost(.exe) are found by name on both.
The cameras come from the loaded configuration. The states are idle, preview of one
camera, preview of all cameras, and preview of all cameras with bandpass filtering of
every stream on the CPU (OpenCV) and on the GPU (CUDA).
"""
import argparse, csv, os, statistics, subprocess, time
import psutil
from scopeone import ScopeOne

NAMES = {"scopeone", "scopeone_driverhost"}


def scopeone_processes():
    out = []
    for p in psutil.process_iter(["name", "cmdline"]):
        cmd = p.info["cmdline"] or [p.info["name"] or ""]
        name = os.path.splitext(os.path.basename(cmd[0]))[0].lower()
        if name in NAMES:
            out.append(p)
    return out


def gpu(index):
    r = subprocess.run(["nvidia-smi", "-i", str(index), "--query-gpu=utilization.gpu",
                        "--format=csv,noheader,nounits"], capture_output=True, text=True)
    return float(r.stdout.strip())


def sample(settle, duration, gpu_index):
    time.sleep(settle)
    procs = {}
    ncpu = psutil.cpu_count()
    rows = []
    t0 = time.time()
    while time.time() - t0 < duration:
        for p in scopeone_processes():
            if p.pid not in procs:
                procs[p.pid] = p
                p.cpu_percent(None)
        time.sleep(1)
        cpu = 0.0
        for pid, p in list(procs.items()):
            try:
                cpu += p.cpu_percent(None)
            except psutil.NoSuchProcess:
                del procs[pid]
        rows.append((cpu / ncpu, gpu(gpu_index)))
    return [statistics.median(c) for c in zip(*rows[1:])], len(procs)


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--camera", help="camera for the single preview, default the first")
    a.add_argument("--settle", type=float, default=10)
    a.add_argument("--duration", type=float, default=60)
    a.add_argument("--gpu", type=int, default=0, help="nvidia-smi GPU index")
    a.add_argument("--out", default="resource_use.csv")
    args = a.parse_args()

    s = ScopeOne()
    cams = s.camera_ids()
    cam = args.camera or cams[0]
    state = s.processing_state()
    saved = [(m["kind"], m["parameters"]) for m in state["modules"]]

    s.stop_preview("All")
    s.set_realtime_processing(False)
    for m in reversed(state["modules"]):
        s.remove_processing_module(m["index"])

    def bandpass(module):
        def enter():
            s.set_realtime_processing(False)
            for m in reversed(s.processing_modules()):
                s.remove_processing_module(m["index"])
            s.add_processing_module(module, {"output_mode": 2, "min_feature_size": 2, "max_feature_size": 10})
            # an empty source sends the frames of every camera through the pipeline
            s.set_realtime_processing(True, "")
        return enter

    states = [("idle", [], lambda: None),
              ("preview " + cam, [cam], lambda: s.start_preview(cam)),
              ("preview all", cams, lambda: s.start_preview("All")),
              ("preview all + bandpass CPU", cams, bandpass("frequency_domain_filter")),
              ("preview all + bandpass GPU", cams, bandpass("cuda.frequency_filter"))]
    results = []
    for name, used, enter in states:
        enter()
        (cpu, g), n = sample(args.settle, args.duration, args.gpu)
        results.append([name, len(used), n, f"{cpu:.1f}", f"{g:.0f}"])
        print(f"{name:30s} cpu {cpu:5.1f} %  gpu {g:4.0f} %", flush=True)

    s.set_realtime_processing(False)
    s.stop_preview("All")
    s.remove_processing_module(0)
    for kind, par in saved:
        s.add_processing_module(kind, par)

    with open(args.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["state", "cameras", "processes", "cpu_percent", "gpu_percent"])
        w.writerows(results)


if __name__ == "__main__":
    main()
