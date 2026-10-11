"""Validate the camera-cbuffer layout found in docs/reverse-engineering.md
against a dumped frame, and list every distinct view rendered.

Layout checked (cb1 of VS/PS, float4 registers):
  c189      render size (w, h, 1/w, 1/h)
  c192-195  main camera viewProj, camera-relative, row-vector layout
  c196-199  main camera projection, column-vector layout (rows in registers)
  c270-273  viewProj of the view currently being drawn (same layout as c192)
  c275      camera position (world) subtracted from instance positions

Usage:
    py -3 tools/check_layout.py analysis/coriolis
"""

import json
import os
import sys

import numpy as np


def reg(x, r, n=1):
    return np.array(x[r * 4:(r + n) * 4]).reshape(n, 4)


def proj_info(p):
    """p: 4x4, rows = registers c196-c199 (clip = p @ v)."""
    sx, sy, a, b = p[0, 0], p[1, 1], p[2, 2], p[2, 3]
    if sy == 0:
        return "no projection"
    fov = np.degrees(2 * np.arctan(1 / sy))
    kind = "infinite reverse-Z, near=%g" % b if abs(a) < 1e-9 else "a=%g b=%g" % (a, b)
    return "vfov=%.2f aspect=%.4f %s offcenter=(%g,%g) w=%s" % (
        fov, sy / sx, kind, p[0, 2], p[1, 2], p[3])


def classify(x):
    """Same rule as the add-on (addon/src/camera.cpp): the current viewProj
    c270-c273 is the main camera when its x/y column lengths have the aspect
    of the main projection c196-c199, its w column is a unit vector and its
    near plane matches; another near plane means first-person body or HUD.
    (c192-c195 is not reliable: it differs in flight and in the SRV.)"""
    vp, p = reg(x, 270, 4), reg(x, 196, 4)
    if p[1, 1] <= 0 or p[0, 0] <= 0 or abs(vp[3, 3]) > 1e-6 or vp[3, 2] <= 0:
        return "other view"
    lx, ly, lw = (np.linalg.norm(vp[:3, i]) for i in (0, 1, 3))
    if lx == 0 or abs((ly / lx) / (p[1, 1] / p[0, 0]) - 1) > 0.005 or abs(lw - 1) > 1e-3:
        return "other view"
    zoom = " zoom=%.4f" % (ly / p[1, 1])
    if abs(vp[3, 2] - p[2, 3]) < 1e-6:
        return "MAIN CAMERA" + zoom
    return "main-aligned (other near)" + zoom


def main():
    folder = sys.argv[1]
    with open(os.path.join(folder, "frame.json"), encoding="utf-8") as f:
        frame = json.load(f)
    with open(os.path.join(folder, "cbuffers.json"), encoding="utf-8") as f:
        blobs = json.load(f)
    tex = {t["id"]: t for t in frame["textures"]}

    views = {}
    for a in frame["actions"]:
        sh = a.get("shaders", {}).get("vs")
        if not sh:
            continue
        cb = next((c for c in sh["cb"] if c["slot"] == 1 and "data" in c and c["decl"] >= 4416), None)
        if not cb:
            continue
        x = blobs[cb["data"]]
        if len(x) < 276 * 4:
            continue
        vp = reg(x, 270, 4)
        main_vp = reg(x, 192, 4)
        key = tuple(np.round(vp.flatten(), 4))
        v = views.setdefault(key, {"first": a["eid"], "n": 0, "targets": set(),
                                   "main": classify(x), "x": x, "res": cb["res"]})
        v["n"] += 1
        v["last"] = a["eid"]
        for o in a["outputs"]:
            v["targets"].add(o)
        if a["depth"] is not None:
            v["targets"].add(a["depth"])

    for key, v in sorted(views.items(), key=lambda kv: kv[1]["first"]):
        x = v["x"]
        size = reg(x, 189)[0]
        print("eid %d-%d draws=%d cb1=#%s %s" % (v["first"], v["last"], v["n"], v["res"], v["main"]))
        print("   render size c189 = %dx%d" % (size[0], size[1]))
        print("   main P (c196-199): " + proj_info(reg(x, 196, 4)))
        vp = reg(x, 270, 4)
        print("   view VP c273 = %s  w-column = %s" % (np.round(vp[3], 5), np.round(vp[:, 3], 4)))
        print("   camera pos c275 = %s" % np.round(reg(x, 275)[0][:3], 3))
        names = []
        for t in sorted(v["targets"]):
            d = tex.get(t)
            names.append("#%d %s %dx%d" % (t, d["format"], d["w"], d["h"]) if d else "#%d" % t)
        print("   targets: " + ", ".join(names))


if __name__ == "__main__":
    main()
