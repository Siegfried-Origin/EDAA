"""Print a constant buffer bound at a given event, one float4 register per line.

Usage:
    py -3 tools/cb_dump.py analysis/coriolis EID STAGE SLOT [FIRST_REG [LAST_REG]]
    py -3 tools/cb_dump.py analysis/coriolis 5000 vs 1 190 200
"""

import json
import os
import sys


def find_cb(folder, eid, stage, slot):
    with open(os.path.join(folder, "frame.json"), encoding="utf-8") as f:
        frame = json.load(f)
    with open(os.path.join(folder, "cbuffers.json"), encoding="utf-8") as f:
        blobs = json.load(f)
    # first action at or after eid that has this stage bound
    for a in frame["actions"]:
        sh = a.get("shaders", {}).get(stage)
        if a["eid"] >= eid and sh:
            for cb in sh["cb"]:
                if cb["slot"] == slot:
                    return a, sh, cb, blobs.get(cb.get("data"), [])
            sys.exit("no cb%d at eid %d" % (slot, eid))
    sys.exit("eid %d not found" % eid)


def main():
    folder, eid, stage, slot = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
    a, sh, cb, data = find_cb(folder, eid, stage, slot)
    first = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    last = int(sys.argv[6]) if len(sys.argv) > 6 else len(data) // 4 - 1
    print("eid %d %s  shader %s  cb%d res=%s off=%s size=%s decl=%s" % (
        eid, a["name"], sh["hash"], slot, cb["res"], cb["off"], cb["size"], cb["decl"]))
    for r in range(first, min(last, len(data) // 4 - 1) + 1):
        v = data[r * 4:r * 4 + 4]
        print("c%-4d +%-5d % 14.6g % 14.6g % 14.6g % 14.6g" % ((r, r * 16) + tuple(v)))


if __name__ == "__main__":
    main()
