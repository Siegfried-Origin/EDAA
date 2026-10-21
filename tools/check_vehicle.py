"""How the player's vehicle really moves between two frames (2-frame capture pair).

Uses the t33 instance records (and the t38 bone matrices of skinned records)
of both frames, and the main camera of each, to print for the records near the
camera:
  - their screen motion (truth: each frame's own transform and camera);
  - whether their bones changed;
  - which cbuffer camera the cockpit draws use.

Usage:
    py -3 tools/check_vehicle.py <prev dump> <cur dump> <prev t33.bin> <cur t33.bin> <prev t38.bin> <cur t38.bin>
"""

import json
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from check_layout import reg  # noqa: E402
from check_motion import project  # noqa: E402


def scene_camera(folder):
    """c270-c273 and c275 of the most used scene camera (vertex shader cb1)."""
    with open(os.path.join(folder, "frame.json"), encoding="utf-8") as f:
        frame = json.load(f)
    with open(os.path.join(folder, "cbuffers.json"), encoding="utf-8") as f:
        blobs = json.load(f)
    tex = {t["id"]: t for t in frame["textures"]}
    counts = {}
    for a in frame["actions"]:
        vs = (a.get("shaders") or {}).get("vs")
        if not vs or a["depth"] is None or not a.get("outputs"):
            continue
        for c in vs["cb"]:
            if c["slot"] == 1 and "data" in c and c["decl"] >= 4416:
                d = tex[a["depth"]]
                counts.setdefault(c["data"], [0, (d["w"], d["h"])])[0] += 1
    best = max(counts, key=lambda k: counts[k][0])
    x = blobs[best]
    return reg(x, 270, 4), reg(x, 275)[0][:3], counts[best][1]


def quat(q):
    x, y, z, w = (v * (1.0 / 32768.0) - 1.0 for v in q)
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])


def records(path):
    raw = open(path, "rb").read()
    out = []
    for i in range(len(raw) // 336):
        r = raw[i * 336:(i + 1) * 336]
        if r == bytes(336):
            continue
        base, scale = struct.unpack_from("<If", r, 0)
        q = struct.unpack_from("<4H", r, 8)
        pos = np.array(struct.unpack_from("<3f", r, 16), dtype=np.float64)
        out.append({"index": i, "base": base, "scale": scale, "rot": quat(q), "pos": pos})
    return out


def bone(t38, index):
    m = struct.unpack_from("<12f", t38, index * 48)
    return np.array(m).reshape(3, 4)


def world(rec, local):
    # Vertex shader: rotate (quaternion), scale, then add the record position.
    return rec["pos"] + rec["scale"] * (rec["rot"] @ local)


def main():
    prev_dir, cur_dir, prev_t33, cur_t33, prev_t38, cur_t38 = sys.argv[1:7]
    vp_prev, cam_prev, size = scene_camera(prev_dir)
    vp_cur, cam_cur, _ = scene_camera(cur_dir)
    size = np.array(size, dtype=np.float64)
    print("camera moved %s m" % np.round(cam_cur - cam_prev, 4))
    b_prev, b_cur = open(prev_t38, "rb").read(), open(cur_t38, "rb").read()

    near = lambda recs, cam: [r for r in recs if np.linalg.norm(r["pos"] - cam) < 25]
    prev = near(records(prev_t33), cam_prev)
    cur = near(records(cur_t33), cam_cur)
    print("records within 25 m: %d previous, %d current" % (len(prev), len(cur)))

    # Match: same kind and scale, nearest camera-relative position.
    rows = []
    for c in cur:
        rel = c["pos"] - cam_cur
        cands = [p for p in prev if (p["base"] != 0) == (c["base"] != 0) and abs(p["scale"] - c["scale"]) < 1e-4]
        if not cands:
            continue
        p = min(cands, key=lambda p: np.linalg.norm((p["pos"] - cam_prev) - rel))
        d = np.linalg.norm((p["pos"] - cam_prev) - rel)
        if d > 1.0:
            continue
        # Screen motion of the record origin and of a point 1 m along its x axis.
        motions = []
        for local in (np.zeros(3), np.array([1.0, 0, 0]), np.array([0, 1.0, 0])):
            uv_c, _, wc = project(vp_cur, world(c, local) - cam_cur, size)
            uv_p, _, wp = project(vp_prev, world(p, local) - cam_prev, size)
            motions.append((uv_p - uv_c) * size if wc > 0 and wp > 0 else np.array([np.nan, np.nan]))
        bone_delta = 0.0
        if c["base"] != 0:
            for k in range(4):
                bone_delta = max(bone_delta, float(np.abs(bone(b_cur, c["base"] + k) - bone(b_prev, p["base"] + k)).max()))
        rows.append((np.linalg.norm(rel), c, p, d, motions, bone_delta))

    rows.sort(key=lambda r: r[0])
    print("%6s %5s %6s %6s  %-22s %-22s %-22s %s" % ("dist", "base", "scale", "match", "origin px", "+x px", "+y px", "bones"))
    for dist, c, p, d, m, bd in rows[:60]:
        print("%6.2f %5s %6.3f %6.3f  %-22s %-22s %-22s %s" % (
            dist, "skin" if c["base"] else "0", c["scale"], d, np.round(m[0], 2), np.round(m[1], 2), np.round(m[2], 2),
            "%.4f" % bd if c["base"] else "-"))


if __name__ == "__main__":
    main()
