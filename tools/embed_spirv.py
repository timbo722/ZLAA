"""
embed_spirv.py - assemble our own small shaders and embed them as C arrays.

    python tools/embed_spirv.py <spirv-as> runtime/render/shader/builtin

Assembles every <name>.<stage>.spvasm in the folder (SPIR-V 1.3, as our translated shaders) and
writes builtin_shaders.h with one array spv_<name>_<stage> per shader.
"""
import glob
import os
import struct
import subprocess
import sys
import tempfile


def main():
    spirv_as, folder = sys.argv[1], sys.argv[2]
    parts = ["/*\n * builtin_shaders.h - SPIR-V of our own small shaders. Generated: edit the .spvasm files and run\n"
             " *     python tools/embed_spirv.py <spirv-as> runtime/render/shader/builtin\n */\n"
             "#ifndef BUILTIN_SHADERS_H\n#define BUILTIN_SHADERS_H\n\n#include <stdint.h>\n"]
    for src in sorted(glob.glob(os.path.join(folder, "*.spvasm"))):
        name = os.path.basename(src)[:-len(".spvasm")]
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "out.spv")
            subprocess.run([spirv_as, "--target-env", "vulkan1.1", src, "-o", out], check=True)
            data = open(out, "rb").read()
        words = struct.unpack("<%dI" % (len(data) // 4), data)
        parts.append("\nstatic const uint32_t spv_%s[%d] = {" % (name.replace(".", "_"), len(words)))
        for i in range(0, len(words), 8):
            parts.append("\n    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
        parts.append("\n};\n")
    parts.append("\n#endif\n")
    open(os.path.join(folder, "builtin_shaders.h"), "w").write("".join(parts))


if __name__ == "__main__":
    main()
