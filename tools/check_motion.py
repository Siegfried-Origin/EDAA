"""Validate the motion vector math of addon/shaders/motion.hlsl on real data.

Uses two consecutive frames (dumps of a 2-frame capture pair) and static
instances of the t33 buffer (bit-identical position in both frames):
  ground truth: uv_prev = project(VP_prev, P - cam_prev), uv_cur = project(VP_cur, P - cam_cur)
  shader math:  from (uv_cur, depth_cur) rebuild P with inverse(VP_cur), move by
                the camera delta, project with VP_prev
and prints both motions in pixels.

Usage:
    py -3 tools/check_motion.py analysis/cockpit_turn-frame6427 analysis/cockpit_turn-frame6428 \
        prev_t33.bin cur_t33.bin
"""

import json
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from check_layout import classify, reg  # noqa: E402


def main_camera(folder):
    """c270-c273 (row-vector viewProj) and c275 of the most used main camera."""
    with open(os.path.join(folder, "frame.json"), encoding="utf-8") as f:
        frame = json.load(f)
    with open(os.path.join(folder, "cbuffers.json"), encoding="utf-8") as f:
        blobs = json.load(f)
    tex = {t["id"]: t for t in frame["textures"]}
    counts = {}
    for a in frame["actions"]:
        sh = a.get("shaders", {}).get("vs")
        if not sh or a["depth"] is None:
            continue
        cb = next((c for c in sh["cb"] if c["slot"] == 1 and "data" in c and c["decl"] >= 4416), None)
        if not cb or not classify(blobs[cb["data"]]).startswith("MAIN"):
            continue
        d = tex[a["depth"]]
        counts.setdefault(cb["data"], [0, (d["w"], d["h"])])[0] += 1
    best = max(counts, key=lambda k: counts[k][0])
    x = blobs[best]
    return reg(x, 270, 4), reg(x, 275)[0][:3], counts[best][1]


def project(vp, p_rel, size):
    clip = np.append(p_rel, 1.0) @ vp
    ndc = clip[:2] / clip[3]
    uv = np.array([ndc[0] * 0.5 + 0.5, 0.5 - ndc[1] * 0.5])
    return uv, clip[2] / clip[3], clip[3]


def shader_motion(vp_cur, vp_prev, cam_cur, cam_prev, uv, depth):
    """Same math as ps_motion (no jitter)."""
    ndc = np.array([uv[0] * 2 - 1, 1 - uv[1] * 2, depth, 1.0])
    h = ndc @ np.linalg.inv(vp_cur)
    hp = np.append(h[:3] + (cam_cur - cam_prev) * h[3], h[3])
    c = hp @ vp_prev
    prev_ndc = c[:2] / c[3]
    return np.array([prev_ndc[0] * 0.5 + 0.5, 0.5 - prev_ndc[1] * 0.5]) - uv


def static_positions(prev_bin, cur_bin):
    def recs(path):
        raw = open(path, "rb").read()
        out = {}
        for i in range(len(raw) // 336):
            r = raw[i * 336:(i + 1) * 336]
            if r != bytes(336):
                out[r[16:28]] = r
        return out
    a, b = recs(prev_bin), recs(cur_bin)
    return [np.array(struct.unpack("<3f", k), dtype=np.float64) for k in b if k in a]


def main():
    prev_dir, cur_dir, prev_bin, cur_bin = sys.argv[1:5]
    vp_prev, cam_prev, size = main_camera(prev_dir)
    vp_cur, cam_cur, _ = main_camera(cur_dir)
    print("render size %s, camera moved %s m" % (size, np.round(cam_cur - cam_prev, 3)))
    size = np.array(size, dtype=np.float64)

    shown = 0
    worst = 0.0
    for p in static_positions(prev_bin, cur_bin):
        uv_cur, depth, w_cur = project(vp_cur, p - cam_cur, size)
        uv_prev, _, w_prev = project(vp_prev, p - cam_prev, size)
        if w_cur <= 0 or w_prev <= 0 or not (0 <= uv_cur[0] <= 1 and 0 <= uv_cur[1] <= 1):
            continue
        truth = (uv_prev - uv_cur) * size
        mine = shader_motion(vp_cur, vp_prev, cam_cur, cam_prev, uv_cur, depth) * size
        worst = max(worst, float(np.abs(truth - mine).max()))
        if shown < 12:
            print("viewZ %9.1f m  pixel %s  truth %s px  shader %s px" % (
                w_cur, np.round(uv_cur * size), np.round(truth, 2), np.round(mine, 2)))
            shown += 1
    print("max difference: %.4f px over all visible static instances" % worst)


if __name__ == "__main__":
    main()
