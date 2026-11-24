"""CPU, memory and GPU use of ScopeOne in four operating states.

Install the client and psutil once (nvidia-smi comes with the NVIDIA driver)
    python -m pip install ScopeOneCore/python/scopeone psutil
Load the configuration in ScopeOne first, then run
    python scripts/resource_use.py --camera "HamamatsuHam_DCAM" --out resource_use.csv
The same command works in PowerShell on Windows and in a Linux shell, and the processes
ScopeOne(.exe) and ScopeOne_DriverHost(.exe) are found by name on both.
The cameras come from the loaded configuration. The states are idle, preview of one
camera, preview of all cameras, and preview of all cameras with a processing module
on every stream.
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
    r = subprocess.run(["nvidia-smi", "-i", str(index), "--query-gpu=utilization.gpu,memory.used",
                        "--format=csv,noheader,nounits"], capture_output=True, text=True)
    u, m = r.stdout.strip().split(",")
    return float(u), float(m) / 1024


def memory(p):
    m = p.memory_info()
    # private bytes on Windows, resident set on Linux
    return getattr(m, "private", m.rss)


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
        cpu = mem = 0.0
        for pid, p in list(procs.items()):
            try:
                cpu += p.cpu_percent(None)
                mem += memory(p)
            except psutil.NoSuchProcess:
                del procs[pid]
        g, gm = gpu(gpu_index)
        rows.append((cpu / ncpu, mem / 1024**3, g, gm))
    return [statistics.median(c) for c in zip(*rows[1:])], len(procs)


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--camera", help="camera for the single preview, default the first")
    a.add_argument("--module", help="processing module, default the CUDA frequency filter if present")
    a.add_argument("--settle", type=float, default=10)
    a.add_argument("--duration", type=float, default=60)
    a.add_argument("--gpu", type=int, default=0, help="nvidia-smi GPU index")
    a.add_argument("--out", default="resource_use.csv")
    args = a.parse_args()

    s = ScopeOne()
    cams = s.camera_ids()
    cam = args.camera or cams[0]
    state = s.processing_state()
    kinds = {m["id"] for m in state["availableModules"]}
    module = args.module or ("cuda.frequency_filter" if "cuda.frequency_filter" in kinds else "frequency_domain_filter")
    saved = [(m["kind"], m["parameters"]) for m in state["modules"]]

    s.stop_preview("All")
    s.set_realtime_processing(False)
    for m in reversed(state["modules"]):
        s.remove_processing_module(m["index"])

    def processing():
        s.add_processing_module(module, {"output_mode": 2, "min_feature_size": 2, "max_feature_size": 10})
        # an empty source sends the frames of every camera through the pipeline
        s.set_realtime_processing(True, "")

    states = [("idle", [], lambda: None),
              ("preview " + cam, [cam], lambda: s.start_preview(cam)),
              ("preview all", cams, lambda: s.start_preview("All")),
              ("preview all + " + module + " on all", cams, processing)]
    results = []
    for name, used, enter in states:
        enter()
        (cpu, mem, g, gm), n = sample(args.settle, args.duration, args.gpu)
        results.append([name, len(used), n, cpu, mem, g, gm])
        print(f"{name:40s} cpu {cpu:5.1f} %  mem {mem:5.2f} GB  gpu {g:4.0f} %  gpu mem {gm:5.2f} GB", flush=True)

    s.set_realtime_processing(False)
    s.stop_preview("All")
    s.remove_processing_module(0)
    for kind, par in saved:
        s.add_processing_module(kind, par)

    idle_gm = results[0][6]
    with open(args.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["state", "cameras", "processes", "cpu_percent", "memory_gb", "gpu_percent", "gpu_memory_gb_above_idle"])
        for r in results:
            w.writerow(r[:3] + [f"{r[3]:.1f}", f"{r[4]:.2f}", f"{r[5]:.0f}", f"{r[6] - idle_gm:.2f}"])


if __name__ == "__main__":
    main()
