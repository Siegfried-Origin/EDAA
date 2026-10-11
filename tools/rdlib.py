"""Shared helpers for RenderDoc replay scripts.

These scripts run inside qrenderdoc's embedded Python (3.8), which provides the
`renderdoc` module. Launch them with tools/run_rd.ps1, which passes arguments
through environment variables (EDAA_CAPTURE, EDAA_OUT, EDAA_ARGS).
"""

import json
import os
import struct
import sys
import traceback

import renderdoc as rd


def env(name, default=None):
    return os.environ.get(name, default)


def log(msg):
    path = env("EDAA_LOG")
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(str(msg) + "\n")


def open_capture(path):
    """Open a capture for local replay. Returns (cap, controller)."""
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(path, "", None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("Cannot open %s: %s" % (path, result))
    if not cap.LocalReplaySupport():
        raise RuntimeError("Capture cannot be replayed locally")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("Cannot replay %s: %s" % (path, result))
    return cap, controller


def close_capture(cap, controller):
    controller.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()


def run(main):
    """Run main(controller) on EDAA_CAPTURE, log errors, then hard-exit qrenderdoc."""
    code = 0
    try:
        cap, controller = open_capture(env("EDAA_CAPTURE"))
        try:
            main(controller)
        finally:
            close_capture(cap, controller)
    except Exception:
        log(traceback.format_exc())
        code = 1
    sys.stdout.flush()
    os._exit(code)


def iter_actions(actions):
    """Depth-first iteration over all actions (draws, dispatches, markers...)."""
    for a in actions:
        yield a
        yield from iter_actions(a.children)


def fnv1_64(data):
    """3DMigoto shader hash: FNV-1 64-bit over the DXBC bytecode, seeded with 0
    (not the usual FNV offset basis). Verified against ShaderUsage.txt."""
    h = 0
    for b in data:
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        h ^= b
    return "%016x" % h


def floats(raw):
    n = len(raw) // 4
    return list(struct.unpack("<%df" % n, raw[: n * 4]))


def write_json(path, obj):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=1)
