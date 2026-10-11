"""Minimal minidump reader: exception, faulting module + RVA, and a raw
stack scan for return addresses inside known modules.

Usage:
    py -3 tools/minidump_info.py crash.dmp [module_substring ...]
"""

import struct
import sys

STREAM_THREAD_LIST = 3
STREAM_MODULE_LIST = 4
STREAM_EXCEPTION = 6
STREAM_MEMORY64_LIST = 9
STREAM_MEMORY_LIST = 5


def read_string(data, rva):
    n = struct.unpack_from("<I", data, rva)[0]
    return data[rva + 4:rva + 4 + n].decode("utf-16-le")


def main():
    path = sys.argv[1]
    wanted = [w.lower() for w in sys.argv[2:]]
    data = open(path, "rb").read()
    sig, ver, nstreams, dir_rva = struct.unpack_from("<IIII", data, 0)
    streams = {}
    for i in range(nstreams):
        t, size, rva = struct.unpack_from("<III", data, dir_rva + i * 12)
        streams[t] = (size, rva)

    modules = []
    size, rva = streams[STREAM_MODULE_LIST]
    n = struct.unpack_from("<I", data, rva)[0]
    for i in range(n):
        base, msize, _, _, name_rva = struct.unpack_from("<QIIII", data, rva + 4 + i * 108)
        modules.append((base, msize, read_string(data, name_rva)))

    def where(addr):
        for base, msize, name in modules:
            if base <= addr < base + msize:
                return "%s+0x%x" % (name.split("\\")[-1], addr - base)
        return None

    # Memory ranges (for the stack scan)
    ranges = []
    if STREAM_MEMORY64_LIST in streams:
        size, rva = streams[STREAM_MEMORY64_LIST]
        count, base_rva = struct.unpack_from("<QQ", data, rva)
        off = base_rva
        for i in range(count):
            start, length = struct.unpack_from("<QQ", data, rva + 16 + i * 16)
            ranges.append((start, length, off))
            off += length
    if STREAM_MEMORY_LIST in streams:
        size, rva = streams[STREAM_MEMORY_LIST]
        count = struct.unpack_from("<I", data, rva)[0]
        for i in range(count):
            start, length, mrva = struct.unpack_from("<QII", data, rva + 4 + i * 16)
            ranges.append((start, length, mrva))

    def read_mem(addr, length):
        for start, rlen, off in ranges:
            if start <= addr and addr + length <= start + rlen:
                return data[off + addr - start:off + addr - start + length]
        return None

    size, rva = streams[STREAM_EXCEPTION]
    thread_id = struct.unpack_from("<I", data, rva)[0]
    code, flags, rec, addr, nparams = struct.unpack_from("<IIQQI", data, rva + 8)
    params = struct.unpack_from("<15Q", data, rva + 8 + 32)
    ctx_size, ctx_rva = struct.unpack_from("<II", data, rva + 8 + 152)
    print("thread %d exception 0x%08x at 0x%x (%s)" % (thread_id, code, addr, where(addr)))
    if code == 0xC0000005:
        print("  access violation: %s address 0x%x" % (["read", "write", "?", "?", "?", "?", "?", "?", "execute"][params[0]], params[1]))

    # x64 CONTEXT: Rsp at 0x98, Rip at 0xF8, general registers from 0x78
    regs = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
    vals = struct.unpack_from("<16Q", data, ctx_rva + 0x78)
    rip = struct.unpack_from("<Q", data, ctx_rva + 0xF8)[0]
    print("  rip 0x%x %s" % (rip, where(rip)))
    print("  " + " ".join("%s=%x" % (r, v) for r, v in zip(regs, vals)))
    rsp = vals[4]

    print("stack scan (return-address candidates):")
    stack = read_mem(rsp, 0x4000) or read_mem(rsp, 0x1000) or b""
    shown = 0
    for i in range(0, len(stack) - 7, 8):
        v = struct.unpack_from("<Q", stack, i)[0]
        w = where(v)
        if w and (not wanted or any(x in w.lower() for x in wanted)):
            print("  [rsp+0x%04x] %s" % (i, w))
            shown += 1
            if shown > 60:
                break

    print("modules of interest:")
    for base, msize, name in modules:
        low = name.lower()
        if any(x in low for x in ("edaa", "dxgi", "d3d11", "reshade", "nv", "amd", "elite")):
            print("  0x%x %8x %s" % (base, msize, name))


if __name__ == "__main__":
    main()
