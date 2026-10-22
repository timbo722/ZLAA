"""addr2sym.py - name guest addresses (e.g. the f_<addr> frames of a backtrace).

    python tools/addr2sym.py <exefs dir> b0c08fc f_b070410 0xadb4aa0 ...

For each address prints the module and the nearest named function at or below it (from the
modules' symbol tables), demangled with c++filt when it is installed. Many of the game's own
functions have no name; those print as module+offset only.
"""
import bisect
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "recompiler"))
from nso_loader import load_exefs  # noqa: E402


def demangle(names):
    if not shutil.which("c++filt"):
        return names
    out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True).stdout
    return out.splitlines()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    img = load_exefs(sys.argv[1], verify_hashes=False, relocate=False)
    starts = sorted(img.named_funcs)
    rows = []
    for arg in sys.argv[2:]:
        addr = int(arg.removeprefix("f_").removeprefix("0x"), 16)
        mod = next((m for m in img.modules if m.base <= addr < m.base + m.size), None)
        where = f"{mod.name}+{addr - mod.base:#x}" if mod else "?"
        i = bisect.bisect_right(starts, addr) - 1
        name = ""
        if i >= 0:
            start = starts[i]
            fname, size = img.named_funcs[start]
            if (size is None or addr < start + size) and (not mod or start >= mod.base):
                name = fname + (f"+{addr - start:#x}" if addr != start else "")
        rows.append((addr, where, name))
    names = demangle([r[2] for r in rows])
    for (addr, where, _), name in zip(rows, names):
        print(f"{addr:#011x}  {where:<22} {name}")


if __name__ == "__main__":
    main()
