"""Scan dumped constant buffers for camera matrices.

Method (see docs/finding-camera-matrices.md):
  1. Slide a 4x4 window over every unique cbuffer, at 16-byte (register)
     alignment, and test both the window and its transpose for the shape of
     a perspective projection (clip.w = +-view.z, mostly zeros).
  2. For every projection P found, test every other 4x4 window X:
       P^-1 * X rigid            -> X is viewProj  (and P^-1 X is the view)
       X * P ~ I                 -> X is inverse projection
       X * (P*V) ~ I             -> X is inverse viewProj
  3. Report where each match lives: shader stage, slot, byte offset, and
     the shader hashes (3DMigoto) of the draws using it.

Usage:
    py -3 tools/find_matrices.py analysis/coriolis [--eid N]
"""

import collections
import json
import os
import sys

import numpy as np

EPS = 1e-4


def load(folder):
    with open(os.path.join(folder, "frame.json"), encoding="utf-8") as f:
        frame = json.load(f)
    with open(os.path.join(folder, "cbuffers.json"), encoding="utf-8") as f:
        blobs = {k: np.array(v, dtype=np.float64) for k, v in json.load(f).items()}
    # Only scan the part of each buffer that some shader declares: whole
    # 64 KB buffers are often bound and the tail holds stale data.
    declared = collections.defaultdict(int)
    for a in frame["actions"]:
        for sh in a.get("shaders", {}).values():
            for cb in sh["cb"]:
                if "data" in cb:
                    declared[cb["data"]] = max(declared[cb["data"]], cb["decl"])
    blobs = {k: v[:declared[k] // 4] for k, v in blobs.items() if declared[k]}
    return frame, blobs


def usage_index(frame):
    """blob hash -> list of (eid, stage, slot, shader hash)."""
    idx = collections.defaultdict(list)
    for a in frame["actions"]:
        for st, sh in a.get("shaders", {}).items():
            for cb in sh["cb"]:
                if "data" in cb:
                    idx[cb["data"]].append((a["eid"], st, cb["slot"], sh["hash"]))
    return idx


def windows(arr):
    """Yield (float offset, 4x4 matrix as stored: row i = register i)."""
    n = len(arr) - len(arr) % 4
    for o in range(0, n - 15, 4):
        m = arr[o:o + 16].reshape(4, 4)
        if np.all(np.isfinite(m)) and np.abs(m).max() < 1e12:
            yield o, m


def as_projection(x):
    """If x (column-vector convention, clip = x @ v) is a perspective
    projection, return its parameters, else None."""
    w = x[3]
    if not (abs(w[0]) < EPS and abs(w[1]) < EPS and abs(w[3]) < EPS and abs(abs(w[2]) - 1) < EPS):
        return None
    sx, sy = x[0, 0], x[1, 1]
    if sx <= 0.01 or sy <= 0.01:
        return None
    if max(abs(x[0, 1]), abs(x[0, 3]), abs(x[1, 0]), abs(x[1, 3]), abs(x[2, 0]), abs(x[2, 1])) > EPS:
        return None
    a, b, s = x[2, 2], x[2, 3], w[2]
    if abs(b) < 1e-9:
        return None
    # view z (along the w sign) at which NDC depth hits 0 and 1
    z0 = -b / a if abs(a) > 1e-12 else float("inf")
    z1 = b / (s - a) if abs(s - a) > 1e-12 else float("inf")
    z0, z1 = z0 * s, z1 * s
    return {
        "handed": "LH (+z forward)" if s > 0 else "RH (-z forward)",
        "reverseZ": abs(z1) < abs(z0),
        "near": min(abs(z0), abs(z1)), "far": max(abs(z0), abs(z1)),
        "vfov_deg": float(np.degrees(2 * np.arctan(1 / sy))),
        "aspect": float(sy / sx),
        "jitter": [float(x[0, 2]), float(x[1, 2])],
        "a": float(a), "b": float(b),
    }


def is_rigid(v, tol=2e-3):
    r = v[:3, :3]
    if np.abs(v[3] - [0, 0, 0, 1]).max() > tol:
        return False
    return np.abs(r @ r.T - np.eye(3)).max() < tol


def describe(o, transposed):
    reg = o // 4
    return "c%d-c%d (byte %d) %s" % (reg, reg + 3, o * 4,
                                     "registers = columns (mul(v,M) / column_major)" if transposed
                                     else "registers = rows (dot per row / row_major)")


def main():
    folder = sys.argv[1]
    frame, blobs = load(folder)
    uses = usage_index(frame)

    # 1. projections
    projs = []
    for h, arr in blobs.items():
        for o, m in windows(arr):
            for transposed, x in ((False, m), (True, m.T)):
                p = as_projection(x)
                if p:
                    projs.append((h, o, transposed, x, p))

    print("== Perspective projections ==")
    unique = {}
    for h, o, t, x, p in projs:
        key = tuple(np.round(x.flatten(), 4))
        unique.setdefault(key, (x, p, []))[2].append((h, o, t))
    # An inverse projection can look like a projection too: keep the one of
    # each (P, P^-1) pair that is used by more draws.
    def n_draws(locs):
        return sum(len(uses.get(h, [])) for h, _, _ in locs)
    for key, (x, p, locs) in list(unique.items()):
        for key2, (x2, _, locs2) in unique.items():
            if key2 != key and n_draws(locs2) > n_draws(locs) and min(
                    np.abs(x @ x2 - np.eye(4)).max(), np.abs(x.T @ x2 - np.eye(4)).max()) < 1e-3:
                p["inverse_of"] = key2
    for x, p, locs in unique.values():
        if "inverse_of" in p:
            continue
        print("P: vfov=%.2f aspect=%.4f near=%.4g far=%.4g reverseZ=%s %s jitter=%s" % (
            p["vfov_deg"], p["aspect"], p["near"], p["far"], p["reverseZ"], p["handed"], p["jitter"]))
        print("   a=%.6g b=%.6g" % (p["a"], p["b"]))
        report_locations(locs, uses)

    # 2. matrices derived from each projection
    print("\n== Matrices related to projections ==")
    for x, p, _ in unique.values():
        if "inverse_of" in p:
            continue
        pinv = np.linalg.inv(x)
        found = collections.defaultdict(list)
        views = {}
        for h, arr in blobs.items():
            for o, m in windows(arr):
                for t, y in ((False, m), (True, m.T)):
                    if np.abs(y @ x - np.eye(4)).max() < 1e-3:
                        found["invProj"].append((h, o, t))
                        continue
                    v = pinv @ y
                    if np.allclose(v, np.eye(4), atol=1e-4):
                        continue  # P itself
                    if is_rigid(v) and np.abs(v[:3, 3]).max() < 1e9:
                        found["viewProj"].append((h, o, t))
                        views[(h, o, t)] = v
        # inverse viewProj: X @ (P V) ~ I for any viewProj found
        vps = {}
        for k in found["viewProj"]:
            h, o, t = k
            m = blobs[h][o:o + 16].reshape(4, 4)
            vps[k] = m.T if t else m
        if vps:
            vp_list = list(vps.values())
            for h, arr in blobs.items():
                for o, m in windows(arr):
                    for t, y in ((False, m), (True, m.T)):
                        for vp in vp_list:
                            if np.abs(y @ vp - np.eye(4)).max() < 5e-3:
                                found["invViewProj"].append((h, o, t))
                                break
        print("P vfov=%.2f near=%.4g:" % (p["vfov_deg"], p["near"]))
        for kind, locs in found.items():
            print("  %s:" % kind)
            report_locations(locs, uses, indent="    ")
            if kind == "viewProj":
                print("    view = P^-1 * viewProj, per location:")
                shown = set()
                for k in locs:
                    for _, st, slot, _ in uses.get(k[0], [])[:1]:
                        if (st, slot, k[1], k[2]) in shown:
                            continue
                        shown.add((st, slot, k[1], k[2]))
                        v = views[k]
                        print("      %s cb%d %s: forward=%s translation=%s" % (
                            st, slot, describe(k[1], k[2]).split(" registers")[0],
                            np.round(v[2, :3], 4), np.round(v[:3, 3], 3)))


def report_locations(locs, uses, indent="   "):
    groups = collections.defaultdict(set)
    for h, o, t in locs:
        for eid, st, slot, sh in uses.get(h, []):
            groups[(st, slot, describe(o, t))].add((eid, sh))
    for (st, slot, d), items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        eids = sorted(e for e, _ in items)
        shaders = sorted({s for _, s in items})
        print("%s%s cb%d %s: %d draws (eid %d..%d), %d shaders e.g. %s" % (
            indent, st, slot, d, len(eids), eids[0], eids[-1], len(shaders), " ".join(shaders[:4])))


if __name__ == "__main__":
    main()
