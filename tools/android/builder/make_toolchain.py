"""
make_toolchain.py - what the app needs to build the game's code on the device (runtime/builder.c), made once:

    python3 tools/android/builder/make_toolchain.py <android dir (~/laar-build/android)>

Downloads, checked against pinned SHA-256 sums:
  - Python for Android (python.org's official "Android embeddable package", PSF license)
  - zig (ziglang.org, MIT): a C compiler and linker in one program, built for aarch64 Linux (runs on Android)
and writes <android dir>/toolchain/out/:
  lib/   libpython3.14.so, libpylaunch.so (the python command: pylaunch.c), libzig.so (zig, renamed):
         programs go in the APK's native-library folder, the only place Android lets an app run them from
  builder.tar   the rest (made the same way from the same files: no dates in it), extracted by the app on first use: Python's standard library (py/stdlib.zip, without
         its tests and GUI tools) and the few extension modules the recompiler needs (py/lib-dynload), the parts of
         zig's library a C compile needs (zig/lib), the recompiler (recompiler/*.py) and the headers the generated
         code is compiled against (runtime/); plus builder.id, which changes when any of it does
Nothing of the game: the player's own dump is translated and compiled on their device.
"""
import hashlib
import io
import os
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

PYTHON = ("https://www.python.org/ftp/python/3.14.10/python-3.14.10-aarch64-linux-android.tar.gz",
          "423c80b957330cf0ebb92b9b20f2294c292144a441bdb0ceb8c69d14928e569b")
ZIG = ("https://ziglang.org/download/0.14.1/zig-aarch64-linux-0.14.1.tar.xz",
       "f7a654acc967864f7a050ddacfaa778c7504a0eca8d2b678839c21eea47c992b")
PY_VERSION = "3.14"

# extension modules the recompiler (or Python's start-up) imports; the rest are left out
DYNLOAD = {"_bisect", "_blake2", "_heapq", "_md5", "_sha1", "_sha2", "_sha3", "_struct", "_random", "_json",
           "_pickle", "_posixsubprocess", "array", "binascii", "math", "select", "fcntl", "zlib"}
STDLIB_SKIP = ("test/", "idlelib/", "tkinter/", "turtledemo/", "ensurepip/", "pydoc_data/", "lib-dynload/",
               "site-packages/", "config-")
ZIG_LIB = ("include", "std", "compiler", "libc/include/generic-musl", "libc/include/aarch64-linux-musl",
           "libc/include/any-linux-any")

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def fetch(url, sha, folder):
    path = os.path.join(folder, os.path.basename(url))
    if not os.path.exists(path):
        print("downloading", url)
        urllib.request.urlretrieve(url, path + ".part")
        os.rename(path + ".part", path)
    h = hashlib.sha256(open(path, "rb").read()).hexdigest()
    if h != sha:
        os.remove(path)
        sys.exit("%s: SHA-256 %s, expected %s (deleted it; run again)" % (path, h, sha))
    return path


def main():
    android = sys.argv[1]
    tc = os.path.join(android, "toolchain")
    out = os.path.join(tc, "out")
    os.makedirs(os.path.join(out, "lib"), exist_ok=True)
    py_tar = tarfile.open(fetch(*PYTHON, tc))
    zig_tar = tarfile.open(fetch(*ZIG, tc))

    # Python: the library, its headers (for the launcher), the standard library, the extension modules
    prefix = "./prefix/"                             # (the package's names start with ./)
    lib = prefix + "lib/python%s/" % PY_VERSION
    stdlib = io.BytesIO()
    tar_buf = io.BytesIO()
    tar = tarfile.open(fileobj=tar_buf, mode="w", format=tarfile.USTAR_FORMAT)   # (plain headers: builder.c reads them)

    def add(name, data, mode=0o644):
        info = tarfile.TarInfo(name)
        info.size = len(data)
        info.mode = mode
        tar.addfile(info, io.BytesIO(data))

    inc = os.path.join(tc, "python-include")
    with zipfile.ZipFile(stdlib, "w", zipfile.ZIP_DEFLATED) as z:
        for m in py_tar.getmembers():
            if not m.isfile():
                continue
            if m.name == prefix + "lib/libpython%s.so" % PY_VERSION:
                open(os.path.join(out, "lib", "libpython%s.so" % PY_VERSION), "wb").write(py_tar.extractfile(m).read())
            elif m.name.startswith(prefix + "include/"):
                dst = os.path.join(inc, m.name[len(prefix + "include/"):])
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                open(dst, "wb").write(py_tar.extractfile(m).read())
            elif m.name.startswith(lib + "lib-dynload/"):
                if os.path.basename(m.name).split(".")[0] in DYNLOAD:
                    add("py/lib-dynload/" + os.path.basename(m.name), py_tar.extractfile(m).read(), 0o755)
            elif m.name == lib + "LICENSE.txt":
                add("py/LICENSE.txt", py_tar.extractfile(m).read())
            elif m.name.startswith(lib) and m.name.endswith(".py"):
                rel = m.name[len(lib):]
                if not rel.startswith(STDLIB_SKIP) and "/tests/" not in rel:   # (a fixed date: same files, same id)
                    z.writestr(zipfile.ZipInfo(rel, date_time=(1980, 1, 1, 0, 0, 0)), py_tar.extractfile(m).read(),
                               zipfile.ZIP_DEFLATED)
    add("py/stdlib.zip", stdlib.getvalue())

    # the python command
    ndk = sorted(os.listdir(os.path.join(android, "sdk", "ndk")))[-1]
    clang = os.path.join(android, "sdk", "ndk", ndk, "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin", "clang")
    subprocess.run([clang, "--target=aarch64-linux-android30", "-O2", "-fPIE", "-pie", "-s",
                    "-I" + os.path.join(inc, "python" + PY_VERSION), os.path.join(REPO, "tools/android/builder/pylaunch.c"),
                    "-L" + os.path.join(out, "lib"), "-lpython" + PY_VERSION, "-o", os.path.join(out, "lib", "libpylaunch.so")],
                   check=True)

    # zig: the program, and the parts of its library a C compile needs
    for m in zig_tar.getmembers():
        parts = m.name.split("/", 1)
        if len(parts) < 2 or not m.isfile():
            continue
        rel = parts[1]
        if rel == "zig":
            open(os.path.join(out, "lib", "libzig.so"), "wb").write(zig_tar.extractfile(m).read())
            os.chmod(os.path.join(out, "lib", "libzig.so"), 0o755)
        elif rel == "LICENSE":
            add("zig/LICENSE", zig_tar.extractfile(m).read())
        elif rel.startswith("lib/") and rel[4:].startswith(ZIG_LIB):
            add("zig/" + rel, zig_tar.extractfile(m).read())

    # the recompiler and the runtime headers the generated code includes
    for f in sorted(os.listdir(os.path.join(REPO, "recompiler"))):
        if f.endswith(".py"):
            add("recompiler/" + f, open(os.path.join(REPO, "recompiler", f), "rb").read())
    for f in ("recomp_runtime.h", "recomp_simd.h", "game_abi.c"):
        add("runtime/" + f, open(os.path.join(REPO, "runtime", f), "rb").read())

    tar.close()
    data = tar_buf.getvalue()
    open(os.path.join(out, "builder.tar"), "wb").write(data)
    open(os.path.join(out, "builder.id"), "w").write(hashlib.sha256(data).hexdigest()[:16] + "\n")
    print("toolchain: %s (builder.tar %.1f MB)" % (out, len(data) / 1e6))


if __name__ == "__main__":
    main()
