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


def recompile_function(img, funcs, start, name, size, stats, comments=True):
    words = [img.word(pc) for pc in range(start, start + size, 4)]
    # Pass 1: which addresses are branch targets inside this function? Is there a BR (jump table)?
    labels, has_br = set(), False
    for k, w in enumerate(words):
        info = branch_info(w, start + 4 * k)
        if info and info[0] in ("b", "cond") and start <= info[1] < start + size:
            labels.add(info[1])
        if info and info[0] == "br":
            has_br = True
    if has_br:  # an indirect jump could land on any instruction
        labels.update(range(start, start + size, 4))

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
        for pc in range(start, start + size, 4):
            body.append(f"    case {hx(pc)}: goto L_{pc:x};")
        body.append("    default: recomp_call_addr(c, jt); return;  /* indirect tail call */")
        body.append("    }")
    # (not in big functions with an indirect-jump dispatch, where every instruction is an entry
    # point and the C compiler spills the locals everywhere, nor in huge ones)
    cache = not os.environ.get("RECOMP_NO_REGISTER_CACHE") and size // 4 <= 3000 and not (has_br and size // 4 > 300)
    if COLLECT_ABI:
        plain = [_REG_RE.sub(_local, l) for l in body]
        text = "\n".join(plain)
        FUNC_INFO[cname(start)] = (follows_abi(plain), re.search(r"\bst64\(c, .*\bX(19|2[0-8])\)", text) is not None,
                                   set(re.findall(r"\b(f_[0-9a-f]+)\(c\)", text)))
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
FUNC_INFO = {}            # first pass: name -> (keeps the convention itself, saves X19-X28, direct callees)
COLLECT_ABI = False


def find_non_abi():
    """NON_ABI from FUNC_INFO: functions that break the convention themselves, and - repeated until
    nothing changes - functions that call one of those without having saved X19-X28 first (so they
    pass their caller's values on to it, like the startup code's trampolines)."""
    NON_ABI.clear()
    NON_ABI.update(n for n, (ok, _, _) in FUNC_INFO.items() if not ok)
    changed = True
    while changed:
        changed = False
        for n, (ok, saves, callees) in FUNC_INFO.items():
            if n not in NON_ABI and not saves and callees & NON_ABI:
                NON_ABI.add(n)
                changed = True
FULL_SYNC_CALLS = ("recomp_trap", "recomp_bad_jump", "recomp_unimplemented")


def follows_abi(lines):
    """Does a function (body with X0..X30 locals) keep the calling convention for X19-X29? Walking
    its statements in order, the first use of each must save it (st64 of it to the stack) or set it
    (X19 = ... not reading it), and one that is set must be saved somewhere (to be restored)."""
    stmts = [t for t in re.split(r"[;{}]", "\n".join(lines)) if t.strip()]
    text = "\n".join(lines)
    for n in range(19, 30):
        r = re.compile(rf"\bX{n}\b")
        save = re.compile(rf"\bst64\(c, .*\bX{n}\)")
        for t in stmts:
            if not r.search(t):
                continue
            if save.search(t):
                break                                   # saved first: fine
            m = re.match(rf"\s*X{n} = (?!=)(.*)", t, re.S)
            if m and not r.search(m.group(1)):
                if not save.search(text):
                    return False                        # changed but never saved
                break                                   # set first: fine
            return False                                # read first: uses its caller's value
    # the condition flags: a function reading them before setting them uses its caller's (callers
    # then store their cached flags first: see cache_registers)
    for t in stmts:
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


def cache_registers(lines):
    lines = [_REG_RE.sub(_local, l) for l in lines]
    used = sorted({n for l in lines for n in re.findall(r"\b(X\d+|XSP)\b", l)},
                  key=lambda n: 99 if n == "XSP" else int(n[1:]))
    if not used:
        return [], lines
    # The condition flags are locals too (FN FZ FC FV): a compare whose flags only a branch reads
    # becomes a plain compare, the rest of its flags never computed. By the calling convention a
    # call doesn't take or keep them, so they are stored only where everything is (jumps out,
    # functions that don't keep the convention: NON_ABI) and reloaded after each call (unused
    # reloads compile away).
    lines = [_local_flags(l) for l in lines]
    flags = any(re.search(r"\bF[NZCV]\b", l) for l in lines)
    flag_store = "c->n = FN; c->z = FZ; c->c = FC; c->v = FV; " if flags else ""
    flag_load = "FN = c->n; FZ = c->z; FC = c->c; FV = c->v; " if flags else ""
    written = [n for n in used if any(re.search(rf"\b{n} = (?!=)", l) for l in lines)]
    field = lambda n: "c->sp" if n == "XSP" else f"c->x[{n[1:]}]"
    store = "".join(f"{field(n)} = {n}; " for n in written)
    callee_saved = lambda n: n == "XSP" or 19 <= int(n[1:]) <= 29
    store = "".join(f"{field(n)} = {n}; " for n in written if n == "XSP" or not callee_saved(n))
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
            res += l[i:m.start()] + "{ " + (store_all + flag_store if full else store) + l[m.start():end] + " " + \
                (load_all if full else load) + flag_load + "}"
            i = end
        l = res + l[i:]
        out.append(re.sub(r"\breturn;", "{ " + store + "return; }", l) if store else l)
    decls = ["    uint64_t " + ", ".join(f"{n} = {field(n)}" for n in used) + ";"]
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
    lines = ["LAARecomp recompiler report", "=" * 40, ""]
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
    args = ap.parse_args()

    t0 = time.time()
    img = load(args.input, args.base)
    fs = discover(img)
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
    for addr, (name, size) in items:
        recompile_function(img, funcs, addr, name, size, Stats(), False)
    COLLECT_ABI = False
    find_non_abi()
    FUNC_INFO.clear()
    print(f"  {len(NON_ABI)} functions don't keep the calling convention ({time.time() - t_abi:.0f} s to find them)",
          file=sys.stderr)
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

    if chunked:
        if chunk:
            flush_chunk()
        # Remove chunks left over from an earlier run that produced more of them
        for old in os.listdir(args.output):
            if old.startswith("funcs_") and old.endswith(".c") and old not in chunk_names:
                os.remove(os.path.join(args.output, old))
        # The list of generated files, for CMakeLists.txt
        write_if_changed(os.path.join(args.output, "sources.cmake"),
                         "set(GAME_SOURCES\n" + "".join(f"    ${{GAME_GEN_DIR}}/{n}\n" for n in chunk_names)
                         + "    ${GAME_GEN_DIR}/functable.c\n)\n")

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
