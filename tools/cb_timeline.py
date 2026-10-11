"""Show how a constant buffer changes through the frame.

Prints one line each time the content bound to STAGE/SLOT changes, with
the event range and a few chosen registers. Useful to tell apart the
different Map()s of a shared per-view buffer (shadow views, main view, HUD...).

Usage:
    py -3 tools/cb_timeline.py analysis/coriolis vs 1 189,196,197,198,199,273,275
"""

import json
import os
import sys


def main():
    folder, stage, slot = sys.argv[1], sys.argv[2], int(sys.argv[3])
    regs = [int(r) for r in sys.argv[4].split(",")] if len(sys.argv) > 4 else [0]
    with open(os.path.join(folder, "frame.json"), encoding="utf-8") as f:
        frame = json.load(f)
    with open(os.path.join(folder, "cbuffers.json"), encoding="utf-8") as f:
        blobs = json.load(f)

    runs = []
    for a in frame["actions"]:
        sh = a.get("shaders", {}).get(stage)
        if not sh:
            continue
        cb = next((c for c in sh["cb"] if c["slot"] == slot and "data" in c), None)
        if not cb:
            continue
        key = (cb["res"], cb["data"])
        if runs and runs[-1]["key"] == key:
            runs[-1]["last"] = a["eid"]
            runs[-1]["n"] += 1
            runs[-1]["targets"].add((tuple(a["outputs"]), a["depth"]))
        else:
            runs.append({"key": key, "first": a["eid"], "last": a["eid"], "n": 1,
                         "targets": {(tuple(a["outputs"]), a["depth"])}})

    for r in runs:
        data = blobs[r["key"][1]]
        print("eid %6d-%-6d n=%-5d res=%s blob=%s targets=%s" % (
            r["first"], r["last"], r["n"], r["key"][0], r["key"][1],
            sorted(r["targets"], key=str)[:3]))
        for reg in regs:
            v = data[reg * 4:reg * 4 + 4]
            if v:
                print("     c%-4d % 12.6g % 12.6g % 12.6g % 12.6g" % ((reg,) + tuple(v)))


if __name__ == "__main__":
    main()
