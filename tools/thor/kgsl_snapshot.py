"""kgsl_snapshot.py <dump> [--ib N] [--regs]: what is in a Qualcomm GPU driver ("kgsl") snapshot.

When the kernel resets the GPU after a hang ("GPU hang detected" in dmesg) it keeps a snapshot of the GPU's state at
/sys/class/kgsl/kgsl-3d0/snapshot/dump: readable over adb, kept until it has been read to the end (only then is the
next hang captured). tools/thor/beach_hang.sh pulls it after each hang.

    adb exec-out cat /sys/class/kgsl/kgsl-3d0/snapshot/dump > snap.bin
    python tools/thor/kgsl_snapshot.py snap.bin            # the sections, the process, where the GPU's command reader was
    python tools/thor/kgsl_snapshot.py snap.bin --ib 2     # the commands of the 2nd indirect buffer (draws, states)
    python tools/thor/kgsl_snapshot.py snap.bin --regs     # every register in the dump

The layout is the kernel's (include/uapi or drivers/gpu/msm/kgsl_snapshot.h): a header, then sections, each
{u16 magic 0xABCD, u16 id, u32 size including this header}.
"""
import struct
import sys
import time

SECTIONS = {0x0101: "OS", 0x0201: "REGS", 0x0202: "REGS_V2", 0x0301: "RB", 0x0302: "RB_V2", 0x0401: "IB", 0x0402: "IB_V2",
            0x0501: "INDEXED_REGS", 0x0502: "INDEXED_REGS_V2", 0x0801: "ISTORE", 0x0901: "DEBUG", 0x0A01: "DEBUGBUS",
            0x0B01: "GPU_OBJECT", 0x0B02: "GPU_OBJECT_V2", 0x0E01: "MEMLIST", 0x0E02: "MEMLIST_V2", 0x1201: "SHADER",
            0x1202: "SHADER_V2", 0x1203: "SHADER_V3", 0x1501: "MVC", 0x1502: "MVC_V2", 0x1503: "MVC_V3", 0x1601: "GMU",
            0x1701: "GMU_MEMORY", 0x1801: "SIDE_DEBUGBUS", 0x1901: "TRACE_BUFFER", 0x1A01: "EVENTLOG", 0xFFFF: "END"}

# Adreno 7xx registers we read (the kernel's gen7_reg.h; the same numbers as the 6xx's for these)
REG = {0x210: "RBBM_STATUS", 0x800: "CP_RB_BASE", 0x801: "CP_RB_BASE_HI", 0x806: "CP_RB_RPTR", 0x807: "CP_RB_WPTR",
       0x928: "CP_IB1_BASE", 0x929: "CP_IB1_BASE_HI", 0x92A: "CP_IB1_REM_SIZE", 0x92B: "CP_IB2_BASE",
       0x92C: "CP_IB2_BASE_HI", 0x92D: "CP_IB2_REM_SIZE", 0x92E: "CP_SDS_BASE", 0x92F: "CP_SDS_BASE_HI",
       0x930: "CP_SDS_REM_SIZE", 0x948: "CP_ROQ_AVAIL_RB", 0x949: "CP_ROQ_AVAIL_IB1", 0x94A: "CP_ROQ_AVAIL_IB2",
       0x94B: "CP_ROQ_AVAIL_SDS"}
STATUS_BITS = ["CP_AHB_CX", "CP_AHB_CP", "CP", "GFX_DBGC", "VBIF", "TSE", "RAS", "RB", "CCU", "A2D", "LRZ", "COM_DCOM",
               "PC_DCALL", "PC_VSD", "TESS", "SP", "VPC", "VFD", "HLSQ"]


def sections(data):
    magic, gpuid, chipid = struct.unpack_from("<III", data, 0)
    if magic >> 16 != 0x504D:
        sys.exit("not a kgsl snapshot (magic %#x)" % magic)
    off = 12
    out = []
    while off + 8 <= len(data):
        m, sid, size = struct.unpack_from("<HHI", data, off)
        if m != 0xABCD or size < 8:
            break
        out.append((sid, off + 8, size - 8))
        if sid == 0xFFFF:
            break
        off += size
    return chipid, out


def regs_of(data, secs):
    """REGS: pairs (address, value); REGS_V2: runs {u32 address | count << ?}: both tried"""
    regs = {}
    for sid, off, size in secs:
        if sid == 0x0201:                                # header: u32 count, then count pairs
            count = struct.unpack_from("<I", data, off)[0]
            for i in range(count):
                a, v = struct.unpack_from("<II", data, off + 4 + 8 * i)
                regs.setdefault(a, v)
        elif sid == 0x0202:                              # runs: (addr with bit 31 = one register) or (addr, count)
            p, end = off, off + size
            while p + 8 <= end:
                a = struct.unpack_from("<I", data, p)[0]
                if a & 0x80000000:
                    regs.setdefault(a & 0x7FFFFFFF, struct.unpack_from("<I", data, p + 4)[0])
                    p += 8
                else:
                    n = struct.unpack_from("<I", data, p + 4)[0]
                    p += 8
                    if n > 0x10000 or p + 4 * n > end:
                        break
                    for i in range(n):
                        regs.setdefault(a + i, struct.unpack_from("<I", data, p + 4 * i)[0])
                    p += 4 * n
    return regs


def ibs_of(data, secs):
    """RB_V2 / IB_V2 sections: (kind, gpu address, dwords, offset of the words)"""
    out = []
    for sid, off, size in secs:
        if sid == 0x0402:                                # u64 gpuaddr, u64 ptbase, u64 size (dwords)
            addr, _pt, n = struct.unpack_from("<QQQ", data, off)
            out.append(("IB", addr, n, off + 24))
        elif sid == 0x0302:                              # int start, end, rbsize, wptr, rptr, count, u32 timestamp_queued,
            start, end, rbsize, wptr, rptr, count = struct.unpack_from("<6i", data, off)   # timestamp_retired, u64 gpuaddr, u32 id
            out.append(("RB", struct.unpack_from("<Q", data, off + 32)[0], count, off + 44))
    return out


# ---- the command stream ("pm4"): type-4 packets write registers, type-7 packets are commands ----
OPCODES = {0x10: "CP_NOP", 0x26: "CP_WAIT_FOR_IDLE", 0x13: "CP_WAIT_FOR_ME", 0x3F: "CP_INDIRECT_BUFFER", 0x37: "CP_INDIRECT_BUFFER_PFE",
           0x38: "CP_DRAW_INDX_OFFSET", 0x22: "CP_DRAW_INDX", 0x29: "CP_DRAW_INDIRECT", 0x28: "CP_DRAW_INDX_INDIRECT", 0x2A: "CP_DRAW_INDIRECT_MULTI",
           0x43: "CP_SET_DRAW_STATE", 0x6D: "CP_SET_PSEUDO_REG", 0x65: "CP_SET_MARKER", 0x46: "CP_EVENT_WRITE", 0x66: "CP_SET_MODE",
           0x52: "CP_SKIP_IB2_ENABLE_GLOBAL", 0x1D: "CP_SKIP_IB2_ENABLE_LOCAL", 0x4A: "CP_LOAD_STATE6_GEOM", 0x34: "CP_LOAD_STATE6_FRAG",
           0x36: "CP_LOAD_STATE6", 0x2B: "CP_BLIT", 0x33: "CP_EXEC_CS", 0x3A: "CP_EXEC_CS_INDIRECT", 0x3E: "CP_REG_TO_MEM",
           0x42: "CP_MEM_TO_REG", 0x3D: "CP_MEM_WRITE", 0x5C: "CP_COND_REG_EXEC", 0x44: "CP_COND_EXEC", 0x55: "CP_SET_SUBDRAW_SIZE",
           0x6C: "CP_REG_TEST", 0x60: "CP_START_BIN", 0x61: "CP_END_BIN", 0x72: "CP_THREAD_CONTROL", 0x21: "CP_SET_BIN_DATA5",
           0x2F: "CP_SET_BIN_DATA5_OFFSET", 0x5F: "CP_SET_VISIBILITY_OVERRIDE", 0x12: "CP_CONTEXT_REG_BUNCH", 0x3B: "CP_REG_WRITE",
           0x16: "CP_WHERE_AM_I", 0x6F: "CP_FIXED_STRIDE_DRAW_TABLE", 0x7E: "CP_RESET_CONTEXT_STATE", 0x54: "CP_WAIT_MEM_WRITES",
           0x70: "CP_WAIT_FOR_ME2", 0x68: "CP_SET_CTXSWITCH_IB", 0x53: "CP_SET_AMBLE", 0x5B: "CP_BV_BR_COUNT_OPS",
           0x5D: "CP_MODIFY_TIMESTAMP", 0x74: "CP_SET_BIN_DATA5_3D", 0x32: "CP_LOAD_STATE4", 0x24: "CP_MEM_CPY", 0x17: "CP_WAIT_REG_MEM",
           0x45: "CP_COND_WRITE5", 0x62: "CP_DRAW_PRED_ENABLE_GLOBAL", 0x63: "CP_DRAW_PRED_ENABLE_LOCAL", 0x64: "CP_DRAW_PRED_SET",
           0x49: "CP_START_SCENARIO?", 0x4B: "CP_RECORD_PFP_TIMESTAMP?"}


def packets(words):
    """(index, kind, name or register, payload words)"""
    i = 0
    while i < len(words):
        w = words[i]
        t = w >> 28
        if t == 7:
            n = w & 0x7FFF
            op = (w >> 16) & 0x7F
            yield i, "cmd", OPCODES.get(op, "CP_%#x" % op), words[i + 1:i + 1 + n]
            i += 1 + n
        elif t == 4:
            n = w & 0x7F
            yield i, "reg", (w >> 8) & 0x7FFFF, words[i + 1:i + 1 + n]
            i += 1 + n
        else:
            yield i, "?", w, []
            i += 1


def main():
    data = open(sys.argv[1], "rb").read()
    chipid, secs = sections(data)
    print("kgsl snapshot, chip %#x, %d bytes, %d sections" % (chipid, len(data), len(secs)))
    count = {}
    for sid, off, size in secs:
        k = SECTIONS.get(sid, "%#x" % sid)
        count.setdefault(k, [0, 0])
        count[k][0] += 1
        count[k][1] += size
    print("  " + ", ".join("%s x%d (%d KB)" % (k, n, b >> 10) for k, (n, b) in count.items()))
    for sid, off, size in secs:
        if sid == 0x0101:
            osid, seconds = struct.unpack_from("<iI", data, off)
            print("  taken %s (osid %#x)" % (time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(seconds)), osid))
            text = data[off:off + size]
            names = [s.decode("latin1") for s in text.split(b"\0") if len(s) >= 4 and all(32 <= c < 127 for c in s)]
            print("  " + " | ".join(names[:6]))
    regs = regs_of(data, secs)
    if "--regs" in sys.argv:
        for a in sorted(regs):
            print("  %#06x %-18s %#010x" % (a, REG.get(a, ""), regs[a]))
        return
    named = {REG[a]: v for a, v in regs.items() if a in REG}
    if "RBBM_STATUS" in named:
        s = named["RBBM_STATUS"]
        print("  RBBM_STATUS %#010x: busy: %s" % (s, " ".join(n for i, n in enumerate(STATUS_BITS) if s >> i & 1)))
    ibs = ibs_of(data, secs)
    for level in (1, 2):
        base = named.get("CP_IB%d_BASE" % level, 0) | named.get("CP_IB%d_BASE_HI" % level, 0) << 32
        rem = named.get("CP_IB%d_REM_SIZE" % level)
        avail = named.get("CP_ROQ_AVAIL_IB%d" % level)
        print("  IB%d: base %#x, %s dwords not yet read by the GPU, ROQ avail %s" % (level, base, rem, avail if avail is None else hex(avail)))
    for k, (kind, addr, n, off) in enumerate(ibs):
        print("  [%d] %s at %#x, %d dwords" % (k, kind, addr, n))
    if "--ib" in sys.argv:
        k = int(sys.argv[sys.argv.index("--ib") + 1])
        kind, addr, n, off = ibs[k]
        words = list(struct.unpack_from("<%dI" % n, data, off))
        for i, what, name, payload in packets(words):
            if what == "cmd":
                print("  %5d  %-26s %s" % (i, name, " ".join("%08x" % p for p in payload[:8]) + (" ..." if len(payload) > 8 else "")))
            elif what == "reg":
                print("  %5d  reg %#06x x%d            %s" % (i, name, len(payload), " ".join("%08x" % p for p in payload[:8]) + (" ..." if len(payload) > 8 else "")))
            else:
                print("  %5d  ? %08x" % (i, name))


if __name__ == "__main__":
    main()
