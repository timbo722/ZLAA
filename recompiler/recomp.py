#!/usr/bin/env python3
"""
recomp.py - statically recompile ARM64 code into C.

Input can be:
  * a linked ARM64 ELF (our test programs; uses the symbol table)
  * a single .nso file
  * an ExeFS folder (rtld, main, subsdk*, sdk): the game

Examples:
  python recompiler/recomp.py build/test.elf -o build/recomp.c --embed-image
  python recompiler/recomp.py D:/LADump/exefs --report build/la_report.txt
  python recompiler/recomp.py D:/LADump/exefs -o build/la_c/ --report build/la_report.txt

With -o ending in ".c" everything goes in one file; otherwise -o is a folder and the
output is split into chunks (the game is far too big for one C file).
"""
import re
import argparse
import os
import sys
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from arm64 import Translator, Unsupported, bl_over_literal, branch_info, hx  # noqa: E402
from discover import discover  # noqa: E402

try:
    import capstone
    _cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
except ImportError:
    _cs = None

STACK_SIZE = 1 << 20          # 1 MiB guest stack for tests
CHUNK_INSNS = 30000           # instructions per chunk file (~1.8 MB of C, ~10 s to compile)
CACHE_MAX_INSNS = 20000       # registers in locals only in functions up to this size (all of the game: the
                              # largest has 11430; uncached, a big one took clang minutes, see cache_registers)


def disasm(word, pc):
    """Human-readable disassembly (capstone is only used for comments and the report)."""
    if not _cs:
        return ""
    for ins in _cs.disasm(word.to_bytes(4, "little"), pc):
        return f"{ins.mnemonic} {ins.op_str}".strip()
    return "<invalid>"


def load(path, base):
    with open(path, "rb") if os.path.isfile(path) else open(os.devnull, "rb") as f:
        magic = f.read(4)
    if magic == b"\x7fELF":
        from elf_loader import load_elf
        return load_elf(path)
    from nso_loader import load_exefs
    return load_exefs(path, base=base)


def write_if_changed(path, text):
    """Leave unchanged files alone, so the build system only recompiles chunks that really changed."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, "w") as f:
        f.write(text)


def cname(addr):
    return f"f_{addr:x}"


class Stats:
    def __init__(self):
        self.instructions = 0
        self.unsupported = Counter()       # mnemonic -> count
        self.unsupported_why = Counter()   # reason -> count
        self.unsupported_funcs = 0
        self.bad_jumps = 0                 # direct jumps to an address that isn't a known function start
        self.unknown_br_funcs = 0          # functions with a BR that could land anywhere (all instructions labels)


class FuncContext:
    def __init__(self, funcs, start, size, labels):
        self.funcs, self.start, self.end, self.labels = funcs, start, start + size, labels

    def local_label(self, addr):
        if self.start <= addr < self.end:
            self.labels.add(addr)
            return f"L_{addr:x}"
        return None

    def func_name(self, addr):
        return cname(addr) if addr in self.funcs else None


# Where a BR goes. The compiler uses it for two things: switch statements (a jump table: the
# targets are in the function, listed in the table) and tail calls through a code pointer loaded
# from memory (PLT stubs, virtual functions). Anything else could land on any instruction: then
# every instruction is a label and the dispatch lists them all (huge functions like that took
# clang minutes each: its loop optimizations see the whole function as one loop).
_WRITE_RE = re.compile(r"c->x\[(\d+)\] = (?!=)")


_WRITES = {}


def _writes(tr, insn, pc):
    """The registers an instruction writes (from its C translation) and that C."""
    if pc not in _WRITES:
        try:
            text = " ".join(tr.translate(insn, pc))
            _WRITES[pc] = ({int(n) for n in _WRITE_RE.findall(text)}, text)
        except Unsupported:
            _WRITES[pc] = (None, "")
    return _WRITES[pc]


_BR_MEMO = {}


def br_targets(img, funcs, start, size, br_pc):
    if br_pc not in _BR_MEMO:                                # (both passes over the functions ask)
        _BR_MEMO[br_pc] = _br_targets(img, funcs, start, size, br_pc)
    return _BR_MEMO[br_pc]


def _br_targets(img, funcs, start, size, br_pc):
    """The jump table's targets (a list), [] for a tail call through a loaded code pointer, None if unknown.
    Two table forms (registers followed back through the function; xT may be set long before):
      older clang (the game): adrp xT; add xT, xT, #lo; ldrsw xE, [xT, xI, lsl #2]; add xD, xE, xT;
        br xD - entries are signed offsets from the table
      newer clang: ... ldrb/ldrh wE, [xT, xI(, lsl #1)]; adr xB, base; add xD, xB, xE, lsl #2; br xD -
        entries are base + 4 * entry
    How many: from the bound check (cmp wI, #N; ... b.hi/b.hs) or an index masked to 2^k values
    when in sight; otherwise (offset tables) entries are read while they land on an instruction of
    the function - all the real ones, maybe a few more (harmless: only more labels)."""
    tr = Translator(FuncContext(funcs, start, size, set()))

    def last_def(reg, before, near=True):
        """(pc, insn) of the last instruction before `before` that writes x<reg>: within 24
        instructions, or back to the function's start"""
        for pc in range(before - 4, max(start, before - 4 * 24) - 4 if near else start - 4, -4):
            w = img.word(pc)
            regs, _ = _writes(tr, w, pc)
            if regs is None:
                return None
            if reg in regs:
                return pc, w
        return None

    d = (img.word(br_pc) >> 5) & 31
    dd = last_def(d, br_pc)
    if not dd:
        return None
    if re.search(rf"c->x\[{d}\] = ld64\(", _writes(tr, dd[1], dd[0])[1]):
        return []                                            # a code pointer from memory: a tail call
    if any("c->x[30] = ld64(" in _writes(tr, img.word(pc), pc)[1] for pc in range(max(start, br_pc - 32), br_pc, 4)):
        return []                                            # after the epilogue (LR restored): a tail call

    def value(reg, before, depth=0):
        """x<reg>'s value at `before` if it is an address made by adrp/adr (+ add #imm, mov), and
        whether that was found nearby (else from far back in the function, past branches: less sure)"""
        if depth > 6:
            return None, False
        dv = last_def(reg, before)
        near = dv is not None
        dv = dv or last_def(reg, before, near=False)
        if not dv:
            return None, False
        pc, w = dv
        if (w & 0x1F000000) == 0x10000000:                   # adrp / adr
            imm = ((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3
            if w >> 31:
                imm <<= 12
                return (pc & ~0xFFF) + (imm - (1 << 33) if imm & (1 << 32) else imm), near
            return pc + (imm - (1 << 21) if imm & (1 << 20) else imm), near
        if (w & 0xFF800000) == 0x91000000:                   # add xD, xN, #imm
            v, n = value((w >> 5) & 31, pc, depth + 1)
            return (None, False) if v is None else (v + (((w >> 10) & 0xFFF) << (12 if (w >> 22) & 1 else 0)), near and n)
        if (w & 0xFFE0FFE0) == 0xAA0003E0:                   # mov xD, xM
            v, n = value((w >> 16) & 31, pc, depth + 1)
            return v, near and n
        return None, False

    def masked(r, before):
        """(the and's source register, its last writer, the mask) if x<r> is <source> & mask"""
        dr = last_def(r, before)
        m = dr and re.search(rf"c->x\[{r}\] = .*c->x\[(\d+)\] & (0x[0-9a-f]+)ull", _writes(tr, dr[1], dr[0])[1])
        return m and (int(m.group(1)), last_def(int(m.group(1)), dr[0]), m.group(2))

    def bound(i, load_pc):
        """how many entries the index x<i> (used at load_pc) can select, and whether that is sure (a
        compare) or a cap (a mask, maybe only a type conversion); (None, False) if not in sight"""
        idx = {i}
        di = last_def(i, load_pc)
        if di and (di[1] & 0x7FE0FFE0) == 0x2A0003E0:        # mov wI, wJ (orr wI, wzr, wJ)
            idx.add((di[1] >> 16) & 31)
        mi = masked(i, load_pc)
        for pc in range(load_pc - 4, max(start, load_pc - 4 * 24) - 4, -4):
            c = img.word(pc)
            if (c & 0x7F80001F) == 0x7100001F and ((c >> 5) & 31 in idx or     # cmp wJ/xJ, #N on the index,
                                                   mi and masked((c >> 5) & 31, pc) == mi):   # or the same and
                n = (c >> 10) & 0xFFF if not (c >> 22) & 1 else ((c >> 10) & 0xFFF) << 12
                for q in range(pc + 4, load_pc, 4):          # its b.cond
                    if (img.word(q) & 0xFF000010) == 0x54000000:
                        cond = img.word(q) & 15
                        count = n + 1 if cond == 8 else n if cond == 2 else None   # b.hi: 0..N; b.hs: 0..N-1
                        return count, count is not None
                return None, False
        mask = int(mi[2], 16) if mi else 0
        if mask and mask < 4096 and not mask & (mask + 1):  # index & (2^k - 1)
            return mask + 1, False
        return None, False

    def read_table(table, count, sure, entry):
        """the targets: `count` entries if sure, else (up to `count`) while they land on the function's
        instructions"""
        targets = []
        for k in range(count or 4096):
            target = entry(table, k)
            if not (start <= target < start + size) or target & 3:
                if sure:
                    return None                              # not this table
                break
            targets.append(target)
        return targets or None

    def offset_entry(table, k):
        off = img.word(table + 4 * k)
        return table + (off - (1 << 32) if off & (1 << 31) else off)

    w = dd[1]
    a, b = (w >> 5) & 31, (w >> 16) & 31
    if (w & 0xFFE0FC00) == 0x8B000800:                       # add xD, xB, xE, lsl #2: newer clang
        de = last_def(b, dd[0])
        base, _ = value(a, dd[0])
        if not de or base is None:
            return None
        if (de[1] & 0xFFE0FC00) == 0x38606800:               # ldrb wE, [xT, xI]
            size_log = 0
        elif (de[1] & 0xFFE0FC00) == 0x78607800:             # ldrh wE, [xT, xI, lsl #1]
            size_log = 1
        else:
            return None
        table, _ = value((de[1] >> 5) & 31, de[0])
        count, sure = bound((de[1] >> 16) & 31, de[0])
        if table is None or not count:
            return None
        def small_entry(t, k):
            word = img.word((t + (k << size_log)) & ~3)
            e = (word >> (8 * ((t + (k << size_log)) & 3))) & (0xFF if size_log == 0 else 0xFFFF)
            return base + 4 * e
        return read_table(table, count, sure, small_entry)
    if (w & 0xFFE0FC00) != 0x8B000000:                       # add xD, xA, xB (no shift): older clang
        return None
    for e, t in ((a, b), (b, a)):
        de = last_def(e, dd[0])
        if not de or (de[1] & 0xFFE0FC00) != 0xB8A07800:     # ldrsw xE, [xB, xI, lsl #2]
            continue
        table, near = value((de[1] >> 5) & 31, de[0])
        if table is not None and value(t, dd[0])[0] != table:   # (the add adds the same table address)
            continue
        count, sure = bound((de[1] >> 16) & 31, de[0])
        if not sure and (table is None or not near):
            return None                                      # (an unsure table address needs a bound)
        if table is None:
            # (the address went through the stack: every table address the function makes whose
            # `count` entries all land on its instructions - by chance none would)
            found = set()
            for pc in range(start, start + size, 4):
                if (img.word(pc) & 0x9F000000) == 0x90000000:
                    v = value(img.word(pc) & 31, pc + 8)[0]
                    if v is not None and v != pc & ~0xFFF:
                        found |= set(read_table(v, count, True, offset_entry) or ())
            return sorted(found) or None
        return read_table(table, count, sure, offset_entry)
    return None


def recompile_function(img, funcs, start, name, size, stats, comments=True):
    words = [img.word(pc) for pc in range(start, start + size, 4)]
    # Pass 1: which addresses are branch targets inside this function? Where do its BRs go?
    labels, has_br, br_cases = set(), False, set()
    for k, w in enumerate(words):
        info = branch_info(w, start + 4 * k)
        if info and info[0] in ("b", "cond") and start <= info[1] < start + size:
            labels.add(info[1])
        if info and info[0] == "br":
            has_br = True
            if br_cases is not None:
                targets = br_targets(img, funcs, start, size, start + 4 * k)
                br_cases = None if targets is None else br_cases | set(targets)
    if has_br and br_cases is None:   # an indirect jump that could land on any instruction
        br_cases = set(range(start, start + size, 4))
        stats.unknown_br_funcs += 1
    labels |= br_cases

    # Pass 2: translate
    ctx = FuncContext(funcs, start, size, labels)
    tr = Translator(ctx)
    body, last_info, func_bad = [], None, False
    for k, insn in enumerate(words):
        pc = start + 4 * k
        if pc in labels:
            body.append(f"L_{pc:x}:;")
        try:
            stmts = tr.translate(insn, pc)
        except Unsupported as e:
            text = disasm(insn, pc)
            stats.unsupported[text.split(" ")[0] or f"0x{insn:08x}"] += 1
            stats.unsupported_why[str(e)] += 1
            func_bad = True
            stmts = [f"recomp_unimplemented(c, {hx(pc)}, 0x{insn:08x}u); /* {e} */"]
        if insn == 0x94000002 and bl_over_literal(img.word, pc) is not None:
            stmts.append("return;  /* the next word is data: the callee returned past it */")
        stats.instructions += 1
        stats.bad_jumps += sum(s.count("recomp_bad_jump") for s in stmts)
        if comments:
            body.append(f"    /* {pc:x}: {disasm(insn, pc)} */")
        body.extend("    " + s for s in stmts)
        last_info = branch_info(insn, pc)
    stats.unsupported_funcs += func_bad

    # If the last instruction can fall through, continue at the next address.
    if not last_info or last_info[0] in ("cond", "bl", "blr"):
        body.append("    " + tr.jump(start + size - 4, start + size))

    if has_br:
        body.append("dispatch:")
        body.append("    switch (jt) {")
        for pc in sorted(br_cases):
            body.append(f"    case {hx(pc)}: goto L_{pc:x};")
        body.append("    default: recomp_call_addr(c, jt); return;  /* indirect tail call */")
        body.append("    }")
    # (not in big functions where an indirect jump could land on any instruction, where every
    # instruction is an entry point and the C compiler spills the locals everywhere, nor in huge ones)
    cache = not os.environ.get("RECOMP_NO_REGISTER_CACHE") and size // 4 <= CACHE_MAX_INSNS and not (len(br_cases) > 300)
    if COLLECT_ABI:
        plain = [_REG_RE.sub(_local, l) for l in body]
        text = "\n".join(plain)
        uses = first_uses(plain)
        FUNC_INFO[cname(start)] = (follows_abi(plain, uses), re.search(r"\bst64\(c, .*\bX(19|2[0-8])\)", text) is not None,
                                   set(re.findall(r"\b(f_[0-9a-f]+)\(c\)", text)), reads_scratch(uses[0]))
    body = [unroll_lanes(l) for l in body]
    decls, body = cache_registers(body) if cache else ([], body)
    out = [f"/* {name} */", f"void {cname(start)}(CPU *c) {{"]
    out.extend(decls)
    if has_br:
        out.append("    uint64_t jt;")
    out.extend(body)
    out.append("}")
    return out


# Guest registers live in local variables inside each function (X0..X30, XSP) instead of the CPU
# struct: the C compiler can't tell that guest memory (c->mem + address) never overlaps the
# struct, so with c->x[n] it stored every register write and reloaded registers after every
# guest memory store. Locals can't overlap memory: they stay in host registers, like native
# code. The struct is brought up to date only where someone else can look: before calls that
# read or change registers (other guest functions, indirect calls, SVCs, error reports) the
# caller-saved registers (X0-X18, X30) this function wrote and SP are stored, and afterwards the
# caller-saved ones it uses are reloaded; the same stores happen before each return. By the
# AArch64 calling convention, X19-X29 and SP come back from a call unchanged, and a function
# doesn't depend on its caller's X19-X29 (it may save and restore them, which leaves our locals
# right). So X19-X29 in the struct can be stale (only crash reports show it). Not every callee
# keeps the convention: jumps into the middle of other code (B to another "function"), indirect
# jumps, and the compiler's outlined fragments (tiny functions that use whatever registers their
# call site had) - so for those all written registers are stored and all used ones reloaded.
# Which functions keep it is found by a first pass over all of them (follows_abi, NON_ABI).
NON_ABI = set()           # functions that read X19-X29 before saving or setting them, or change them unsaved
# By the convention a callee doesn't read the scratch registers X9-X17 (X16/X17 may even be changed on the way, by
# a veneer): before calling one that doesn't (not in SCRATCH_IN), they aren't stored. Fewer stores: less code to run
# and to compile (call sequences were a quarter of the generated C).
SCRATCH_IN = set()
SCRATCH_REGS = {f"X{n}" for n in range(9, 18)}
FUNC_INFO = {}            # first pass: name -> (keeps the convention itself, saves X19-X28, direct callees)
COLLECT_ABI = False


def find_non_abi():
    """NON_ABI from FUNC_INFO: functions that break the convention themselves, and - repeated until
    nothing changes - functions that call one of those without having saved X19-X28 first (so they
    pass their caller's values on to it, like the startup code's trampolines)."""
    NON_ABI.clear()
    NON_ABI.update(n for n, (ok, _, _, _) in FUNC_INFO.items() if not ok)
    changed = True
    while changed:
        changed = False
        for n, (ok, saves, callees, _) in FUNC_INFO.items():
            if n not in NON_ABI and not saves and callees & NON_ABI:
                NON_ABI.add(n)
                changed = True
    # SCRATCH_IN: functions that read a scratch register (X9-X17) their caller set, and - repeated - functions that
    # call one of those (they may pass their caller's value on)
    SCRATCH_IN.clear()
    SCRATCH_IN.update(n for n, (_, _, _, scratch) in FUNC_INFO.items() if scratch)
    changed = True
    while changed:
        changed = False
        for n, (_, _, callees, _) in FUNC_INFO.items():
            if n not in SCRATCH_IN and callees & SCRATCH_IN:
                SCRATCH_IN.add(n)
                changed = True
FULL_SYNC_CALLS = ("recomp_trap", "recomp_bad_jump", "recomp_unimplemented")


_TOK_RE = re.compile(r"\bX(\d+)\b")
_ASSIGN_HEAD_RE = re.compile(r"\s*X(\d+) = (?!=)")
_SAVED_RE = re.compile(r"\bX(\d+)\)")


def first_uses(lines):
    """Walking a function's statements in order (body with X0..X30 locals): how each register is first used -
    'save' (st64 of it: saved to the stack), 'set' (X19 = ... not reading it) or 'read' - and which registers it
    saves anywhere. One pass for all registers (it was a search per register per statement: the slowest part of
    translating)."""
    first, saved_any = {}, set()
    for t in re.split(r"[;{}]", "\n".join(lines)):
        regs = {int(x) for x in _TOK_RE.findall(t)}
        if not regs:
            continue
        saves = set()
        k = t.find("st64(c, ")
        if k >= 0 and (k == 0 or not (t[k - 1].isalnum() or t[k - 1] == "_")):
            saves = {int(x) for x in _SAVED_RE.findall(t[k + 8:].split("\n")[0])}
            saved_any |= saves
        m = _ASSIGN_HEAD_RE.match(t)
        target = int(m.group(1)) if m else None
        rhs = {int(x) for x in _TOK_RE.findall(t[m.end():])} if m else ()
        for n in regs:
            if n not in first:
                first[n] = "save" if n in saves else "set" if n == target and n not in rhs else "read"
    return first, saved_any


def reads_scratch(first):
    """Does a function read one of X9-X17 before setting it (its caller's value)? (first: first_uses)"""
    return any(first.get(n) in ("read", "save") for n in range(9, 18))


def follows_abi(lines, uses=None):
    """Does a function keep the calling convention for X19-X29? The first use of each must save it (st64 of it to
    the stack) or set it, and one that is set must be saved somewhere (to be restored)."""
    first, saved_any = uses or first_uses(lines)
    for n in range(19, 30):
        how = first.get(n)
        if how is None or how == "save":
            continue
        if how == "set" and n in saved_any:
            continue
        return False                                    # read first, or changed and never saved
    # the condition flags: a function reading them before setting them uses its caller's (callers
    # then store their cached flags first: see cache_registers)
    for t in re.split(r"[;{}]", "\n".join(lines)):
        if not t.strip():
            continue
        if _FLAG_FN_RE.search(t):
            break
        if _FLAG_RE.search(t):
            return False
    return True


REG_CALLS = ("recomp_call_addr", "recomp_svc", "recomp_trap", "recomp_bad_jump", "recomp_unimplemented")
_REG_RE = re.compile(r"c->x\[(\d+)\]|c->sp\b")
_CALL_RE = re.compile(r"\b(f_[0-9a-f]+|" + "|".join(REG_CALLS) + r")\(c[,)]")


def _local(m):
    return "XSP" if m.group(0) == "c->sp" else f"X{m.group(1)}"


_FLAG_RE = re.compile(r"\bc->([nzcv])\b")
_FLAG_FNS = {"add64_flags": "add64_fl", "add32_flags": "add32_fl", "logic64_flags": "logic64_fl",
             "logic32_flags": "logic32_fl", "set_nzcv": "set_nzcv_fl", "fcmp": "fcmp_fl"}
_FLAG_FN_RE = re.compile(r"\b(" + "|".join(_FLAG_FNS) + r")\(c, ")


def _local_flags(l):
    l = _FLAG_RE.sub(lambda m: "F" + m.group(1).upper(), l)
    return _FLAG_FN_RE.sub(lambda m: _FLAG_FNS[m.group(1)] + "(&FN, &FZ, &FC, &FV, ", l)


# A vector instruction works on each lane in a small loop (for (int k = 0; k < 4; k++) { r.f[k] = ...; }), and
# math-heavy functions have thousands of them: the C compiler then spent a third of its time rotating loops before
# unrolling every one (funcs_0164: 71 s, the median file 8 s). Written out lane by lane here: the same code.
_LANE_LOOP_RE = re.compile(r"for \(int k = (\d+); k < (\d+); k\+\+\) \{")
_K_RE = re.compile(r"\bk\b")


def unroll_lanes(line):
    out, i = "", 0
    while True:
        m = _LANE_LOOP_RE.search(line, i)
        if not m:
            return out + line[i:]
        depth, j = 1, m.end()                        # the body: up to the matching }
        while j < len(line) and depth:
            depth += {"{": 1, "}": -1}.get(line[j], 0)
            j += 1
        body = line[m.end():j - 1]
        if depth or _LANE_LOOP_RE.search(body):      # (unbalanced or nested: left as it is)
            out += line[i:j]
        else:
            out += line[i:m.start()] + "{" + "".join("{" + _K_RE.sub(str(k), body) + "}"   # (each lane its own block:
                                                     for k in range(int(m.group(1)), int(m.group(2)))) + "}"   # locals in it)
        i = j


# Guest memory's base in a local too (M = c->mem): read through c it is loaded again after every store to guest
# memory (see mld64 in recomp_runtime.h). The atomic and vector helpers keep c (they use more of it).
_MEM_RE = re.compile(r"\b(ld|st)(8|16|32|64)\(c, ")
_USED_RE = re.compile(r"\b(X\d+|XSP)\b")
_ASSIGN_RE = re.compile(r"\b(X\d+|XSP) = (?!=)")
_LFLAG_RE = re.compile(r"\bF[NZCV]\b")


def cache_registers(lines):
    lines = [_MEM_RE.sub(r"m\1\2(M, ", _REG_RE.sub(_local, l)) for l in lines]
    mem_decl = ["    uint8_t *const M = c->mem;"] if any("(M, " in l for l in lines) else []
    used = sorted({n for l in lines for n in _USED_RE.findall(l)}, key=lambda n: 99 if n == "XSP" else int(n[1:]))
    if not used:
        return mem_decl, lines
    # The condition flags are locals too (FN FZ FC FV): a compare whose flags only a branch reads
    # becomes a plain compare, the rest of its flags never computed. By the calling convention a
    # call doesn't take or keep them, so they are stored only where everything is (jumps out,
    # functions that don't keep the convention: NON_ABI) and reloaded after each call (unused
    # reloads compile away).
    lines = [_local_flags(l) for l in lines]
    flags = any(_LFLAG_RE.search(l) for l in lines)
    flag_store = "c->n = FN; c->z = FZ; c->c = FC; c->v = FV; " if flags else ""
    flag_load = "FN = c->n; FZ = c->z; FC = c->c; FV = c->v; " if flags else ""
    assigned = {n for l in lines for n in _ASSIGN_RE.findall(l)}
    written = [n for n in used if n in assigned]
    field = lambda n: "c->sp" if n == "XSP" else f"c->x[{n[1:]}]"
    callee_saved = lambda n: n == "XSP" or 19 <= int(n[1:]) <= 29
    store = "".join(f"{field(n)} = {n}; " for n in written if n == "XSP" or not callee_saved(n))
    store_args = "".join(f"{field(n)} = {n}; " for n in written                 # (for callees not in SCRATCH_IN)
                         if n == "XSP" or not (callee_saved(n) or n in SCRATCH_REGS))
    load = "".join(f"{n} = {field(n)}; " for n in used if not callee_saved(n))
    store_all = "".join(f"{field(n)} = {n}; " for n in written)
    load_all = "".join(f"{n} = {field(n)}; " for n in used)
    out = []
    for l in lines:
        # wrap each register-visible call: store before, reload after (scan for its closing ")")
        res, i = "", 0
        for m in _CALL_RE.finditer(l):
            if m.start() < i:
                continue
            depth, j = 0, l.index("(", m.start())
            while j < len(l):
                if l[j] == "(":
                    depth += 1
                elif l[j] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            end = j + 1
            if end < len(l) and l[end] == ";":
                end += 1
            callee, jump = m.group(1), l[end:].lstrip().startswith("return;")
            full = jump or callee in FULL_SYNC_CALLS or callee in NON_ABI
            pre = store_all + flag_store if full else \
                store_args if callee.startswith("f_") and callee not in SCRATCH_IN else store
            res += l[i:m.start()] + "{ " + pre + l[m.start():end] + " " + \
                (load_all if full else load) + flag_load + "}"
            i = end
        l = res + l[i:]
        out.append(re.sub(r"\breturn;", "{ " + store + "return; }", l) if store else l)
    decls = mem_decl + ["    uint64_t " + ", ".join(f"{n} = {field(n)}" for n in used) + ";"]
    if flags:
        decls.append("    uint8_t FN = c->n, FZ = c->z, FC = c->c, FV = c->v;")
    return decls, out


def emit_image(img):
    out = ["", "/* ---- program memory image (test builds only) ---- */", "#include <stdlib.h>"]
    for idx, seg in enumerate(img.segments):
        data = bytes(seg.data).rstrip(b"\0")   # trailing zeros (.bss) come from calloc
        out.append(f"static const uint8_t seg{idx}[{max(len(data), 1)}] = {{")
        for k in range(0, len(data), 24):
            out.append("    " + ",".join(str(b) for b in data[k:k + 24]) + ",")
        out.append("};")
    mem_size = ((img.mem_end() + 0xFFFF) & ~0xFFFF) + STACK_SIZE
    out += [
        f"const uint64_t recomp_mem_size = {hx(mem_size)};",
        f"const uint64_t recomp_stack_top = {hx(mem_size - 16)};",
        "void recomp_init(CPU *c) {",
        "    memset(c, 0, sizeof *c);",
        "    c->mem = calloc(1, recomp_mem_size);",
        "    c->mem_size = recomp_mem_size;",
    ]
    for idx, seg in enumerate(img.segments):
        n = len(bytes(seg.data).rstrip(b"\0"))
        if n:
            out.append(f"    memcpy(c->mem + {hx(seg.vaddr)}, seg{idx}, {n});")
    out += ["    c->sp = recomp_stack_top;", "    c->excl_addr = ~0ull;", "}"]
    return out


# Build IDs of Link's Awakening's main executable (source: https://zeldamods.org/las/Versions)
KNOWN_BUILDS = {
    "ae16f71e002af8cb059a9a74c4d90f34ba984892": "Link's Awakening v1.0.0  <- the version we target",
    "909e904af78ac1b8deefe97ab2ccdb51968f0ec7": "Link's Awakening v1.0.1  (update applied! we target v1.0.0)",
}


def identify(build_id):
    return next((v for k, v in KNOWN_BUILDS.items() if build_id.hex().startswith(k)), None)


def write_report(path, img, fs, stats, seconds):
    lines = ["ZLAA recompiler report", "=" * 40, ""]
    lines.append("Modules:")
    for m in img.modules:
        bid = m.build_id.rstrip(bytes(1)).hex()
        lines.append(f"  {m.name:<10} base 0x{m.base:x}  size 0x{m.size:x}  "
                     f"text 0x{m.text[1] - m.text[0]:x}  build id {bid}")
        known = identify(m.build_id)
        if known:
            lines.append(f"             = {known}")
    lines.append("")
    counts = fs.counts()
    lines.append(f"Functions found: {len(fs.funcs)}")
    for src, n in counts.items():
        lines.append(f"  from {src:<9} {n}")
    lines.append("")
    total_bad = sum(stats.unsupported.values())
    pct = 100.0 * (stats.instructions - total_bad) / max(stats.instructions, 1)
    lines.append(f"Instructions: {stats.instructions}  translated: {stats.instructions - total_bad} ({pct:.2f}%)")
    lines.append(f"Functions containing an unsupported instruction: {stats.unsupported_funcs} of {len(fs.funcs)}")
    lines.append(f"Jumps into the middle of another function (need handling later): {stats.bad_jumps}")
    lines.append(f"Functions with a BR that could land anywhere (every instruction a label): {stats.unknown_br_funcs}")
    lines.append("")
    lines.append("Unsupported instructions by category:")
    for why, n in stats.unsupported_why.most_common():
        lines.append(f"  {n:>9}  {why}")
    lines.append("")
    lines.append("Unsupported instructions by mnemonic:")
    for mnem, n in stats.unsupported.most_common():
        lines.append(f"  {n:>9}  {mnem}")
    if img.notes:
        lines += ["", "Loader notes:"] + [f"  - {n}" for n in img.notes]
    lines += ["", f"Took {seconds:.1f} s"]
    text = "\n".join(lines) + "\n"
    if path:
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
    return text


def main():
    ap = argparse.ArgumentParser(description="Recompile ARM64 code (ELF, NSO or ExeFS folder) into C")
    ap.add_argument("input")
    ap.add_argument("-o", "--output", help="output .c file, or a folder for chunked output")
    ap.add_argument("--report", help="write a coverage report here")
    ap.add_argument("--embed-image", action="store_true", help="embed memory image + recomp_init (tests)")
    ap.add_argument("--symbols", help="write a list of function name -> address here")
    ap.add_argument("--no-comments", action="store_true", help="don't put disassembly comments in the C")
    ap.add_argument("--chunk-insns", type=int, default=CHUNK_INSNS,
                    help="chunked output: about this many instructions per .c file")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x8000000, help="load address for NSO modules")
    ap.add_argument("--progress", action="store_true",
                    help="print 'progress <0..1>' lines (stdout) for a progress bar (the app's on-device build)")
    args = ap.parse_args()

    def progress(x):
        if args.progress:
            print(f"progress {x:.3f}", flush=True)

    # (the shares are the steps' times: loading ~2%, finding the functions ~6%, the first pass ~57%, writing the C ~35%)
    t0 = time.time()
    img = load(args.input, args.base)
    progress(0.02)
    fs = discover(img)
    progress(0.08)
    funcs = fs.funcs
    stats = Stats()
    comments = not args.no_comments

    single = args.output and args.output.endswith(".c")
    chunked = args.output and not single
    if chunked:
        os.makedirs(args.output, exist_ok=True)
        write_if_changed(os.path.join(args.output, "funcs.h"),
                         '#include "recomp_runtime.h"\n' + "".join(f"void {cname(a)}(CPU *c);\n" for a in funcs))

    # Chunks are cut by instruction count, not function count: function sizes vary wildly,
    # and a parallel build waits for its slowest file, so similar-sized chunks build fastest.
    chunk_names, chunk, chunk_insns = [], [], 0

    def flush_chunk():
        nonlocal chunk, chunk_insns
        name = f"funcs_{len(chunk_names):04d}.c"
        write_if_changed(os.path.join(args.output, name),
                         '/* Generated by recomp.py - do not edit */\n#include "funcs.h"\n\n' + "".join(chunk))
        chunk_names.append(name)
        chunk, chunk_insns = [], 0

    out_f, items = None, list(funcs.items())
    if single:
        out_f = open(args.output, "w")
        out_f.write(f"/* Generated by recomp.py from {os.path.basename(args.input)} - do not edit */\n")
        out_f.write('#include "recomp_runtime.h"\n\n')
        out_f.writelines(f"void {cname(a)}(CPU *c);\n" for a in funcs)
    # First pass: which functions keep the calling convention
    global COLLECT_ABI
    t_abi = time.time()
    COLLECT_ABI = True
    for k, (addr, (name, size)) in enumerate(items):
        recompile_function(img, funcs, addr, name, size, Stats(), False)
        if k % 5000 == 0:
            progress(0.08 + 0.57 * k / len(items))
    COLLECT_ABI = False
    find_non_abi()
    FUNC_INFO.clear()
    print(f"  {len(NON_ABI)} functions don't keep the calling convention, {len(SCRATCH_IN)} may read a scratch "
          f"register their caller set ({time.time() - t_abi:.0f} s to find them)", file=sys.stderr)
    for k, (addr, (name, size)) in enumerate(items):
        code = "\n".join(recompile_function(img, funcs, addr, name, size, stats, comments)) + "\n\n"
        if out_f:
            out_f.write(code)
        elif chunked:
            chunk.append(code)
            chunk_insns += size // 4
            if chunk_insns >= args.chunk_insns:
                flush_chunk()
        if len(items) > 20000 and k % 20000 == 0 and k:
            print(f"  ... {k}/{len(items)} functions", file=sys.stderr)
        if k % 5000 == 0:
            progress(0.65 + 0.34 * k / len(items))

    if chunked:
        if chunk:
            flush_chunk()
        # Remove chunks left over from an earlier run that produced more of them
        for old in os.listdir(args.output):
            if old.startswith("funcs_") and old.endswith(".c") and old not in chunk_names:
                os.remove(os.path.join(args.output, old))
        # The list of generated files, for CMakeLists.txt, and the build id of main (the first 8
        # bytes in hex): the game library is libgame-<that>.so, how the runtime finds it (game_lib.c)
        main_id = next((m.build_id[:8].hex().upper() for m in img.modules if m.name == "main"), "NOMAIN")
        write_if_changed(os.path.join(args.output, "sources.cmake"),
                         "set(GAME_SOURCES\n" + "".join(f"    ${{GAME_GEN_DIR}}/{n}\n" for n in chunk_names)
                         + "    ${GAME_GEN_DIR}/functable.c\n)\n" + f"set(GAME_BUILD_ID {main_id})\n")

    if args.output:
        table = ['#include "funcs.h"\n'] if chunked else []
        table.append("const RecompFuncEntry recomp_func_table[] = {\n")
        table += [f"    {{ {hx(a)}, {cname(a)} }},  /* {n} */\n" for a, (n, _) in funcs.items()]
        table.append("};\n")
        table.append(f"const uint64_t recomp_func_count = {len(funcs)};\n")
        if chunked:
            table.append("const RecompModuleInfo recomp_modules[] = {\n")
            for m in img.modules:
                bid = ",".join(f"0x{b:02x}" for b in m.build_id[:32].ljust(32, bytes(1)))
                table.append(f'    {{ "{m.name}", {hx(m.base)}, {hx(m.size)}, {{ {bid} }} }},\n')
            table.append("};\n")
            table.append(f"const uint64_t recomp_module_count = {len(img.modules)};\n")
        if args.embed_image:
            table.append("\n".join(emit_image(img)) + "\n")
        if chunked:
            write_if_changed(os.path.join(args.output, "functable.c"), "".join(table))
        else:
            out_f.writelines(table)
            out_f.close()

    if args.symbols:
        with open(args.symbols, "w") as f:
            for a, (n, _) in funcs.items():
                f.write(f"{n} {a:#x}\n")

    report = write_report(args.report, img, fs, stats, time.time() - t0)
    total_bad = sum(stats.unsupported.values())
    if args.report:
        print(f"Report written to {args.report}")
        print("\n".join(report.splitlines()[2:14]))
    else:
        print(f"{args.input}: {len(funcs)} functions, {stats.instructions - total_bad} instructions translated, "
              f"{total_bad} unsupported")
        for mnem, count in stats.unsupported.most_common(20):
            print(f"    unsupported: {mnem} x{count}")
    return 1 if total_bad else 0


if __name__ == "__main__":
    sys.exit(main())
