import sys
import os

if len(sys.argv) < 3:
    print("Usage: embed_static.py <output.h> <input files...>")
    sys.exit(1)

out_file = sys.argv[1]
in_files = [sys.argv[i] for i in range(2, len(sys.argv))]

with open(out_file, "w") as f:
    f.write("#pragma once\n\n#include <cstddef>\n\n")
    for in_file in in_files:
        basename = os.path.basename(in_file)
        var_name = basename.replace(".", "_").replace("-", "_")

        with open(in_file, "rb") as bf:
            data = bf.read()

        f.write(f"const unsigned char {var_name}[] = {{\n")
        f.write("    " + ", ".join(f"0x{b:02x}" for b in data))
        f.write(", 0x00\n};\n")
        f.write(f"const size_t {var_name}_len = sizeof({var_name}) - 1;\n\n")
