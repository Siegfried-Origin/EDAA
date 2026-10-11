"""Summarise a dumped frame (rd_dump_frame.py output) as a list of passes.

A "pass" is a run of consecutive actions writing the same render targets
and depth buffer. Usage:
    py -3 tools/frame_passes.py analysis/coriolis [--draws]
"""

import json
import os
import sys


def load(folder):
    with open(os.path.join(folder, "frame.json"), encoding="utf-8") as f:
        return json.load(f)


def tex_label(tex, rid):
    t = tex.get(rid)
    if not t:
        return "#%s" % rid
    size = "%dx%d" % (t["w"], t["h"])
    if t["array"] > 1:
        size += "[%d]" % t["array"]
    if t["msaa"] > 1:
        size += " x%dMSAA" % t["msaa"]
    return "#%d %s %s" % (rid, t["format"], size)


def kind(a):
    f = a["flags"]
    # renderdoc.ActionFlags bit values
    if f & 0x0001: return "clear"
    if f & 0x0002: return "draw"
    if f & 0x0004: return "dispatch"
    if f & 0x0400: return "copy"
    if f & 0x0800: return "resolve"
    if f & 0x1000: return "genmips"
    if f & 0x0100: return "present"
    return "other"


def passes(frame):
    tex = {t["id"]: t for t in frame["textures"]}
    out, cur = [], None
    for a in frame["actions"]:
        k = kind(a)
        key = (k if k in ("copy", "resolve", "dispatch", "clear") else "draw",
               tuple(a["outputs"]), a["depth"])
        if cur is None or cur["key"] != key:
            cur = {"key": key, "first": a["eid"], "last": a["eid"], "n": 0,
                   "vs": set(), "ps": set(), "cs": set(), "names": set(), "acts": []}
            out.append(cur)
        cur["last"] = a["eid"]
        cur["n"] += 1
        cur["names"].add(a["name"].split("(")[0])
        cur["acts"].append(a)
        for st in ("vs", "ps", "cs"):
            sh = a.get("shaders", {}).get(st)
            if sh:
                cur[st].add(sh["hash"])
    return out, tex


def main():
    folder = sys.argv[1]
    show_draws = "--draws" in sys.argv
    frame = load(folder)
    ps, tex = passes(frame)
    for p in ps:
        k, outs, depth = p["key"]
        print("EID %6d-%-6d %-8s n=%-4d" % (p["first"], p["last"], k, p["n"]))
        for o in outs:
            print("      RT  " + tex_label(tex, o))
        if depth is not None:
            print("      DS  " + tex_label(tex, depth))
        if k == "copy":
            for a in p["acts"]:
                s, d = a.get("copy", [None, None])
                print("      %s: %s -> %s" % (a["name"], tex_label(tex, s), tex_label(tex, d)))
        if p["cs"]:
            print("      cs: " + " ".join(sorted(p["cs"])))
        if show_draws and k == "draw":
            for a in p["acts"]:
                sh = a.get("shaders", {})
                vp = a.get("viewport", [0] * 6)
                print("        %6d %-40s vs=%s ps=%s vp=%gx%g depth=%s/%s" % (
                    a["eid"], a["name"][:40], sh.get("vs", {}).get("hash"),
                    sh.get("ps", {}).get("hash"), vp[2], vp[3], a.get("depthfunc"),
                    "w" if a.get("depthwrite") else "-"))


if __name__ == "__main__":
    main()
