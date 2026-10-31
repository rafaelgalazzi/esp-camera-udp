"""Adapt ESP-IDF 5.5.5 compile response files to PlatformIO 6.13/SCons.

PlatformIO's ESP-IDF adapter strips a trailing quote from a compile fragment
that consists solely of @".../toolchain/asmflags" before calling ParseFlags.
SCons also classifies @response files as linker flags, losing compile options
such as -mlongcalls. Expand only the three generated compile-response files
inside this build directory before SCons classifies their contents. This applies
to cloned component/bootloader environments and leaves installed tools intact.
"""
from pathlib import Path
import re
from SCons.Environment import Base

Import("env")  # Provided by PlatformIO/SCons.


def parse_flags_with_asm_response(self, *flags, **kwargs):
    build_root = Path(self.subst("$BUILD_DIR")).resolve()
    compile_files = {"cflags", "cxxflags", "asmflags"}

    def expand(match):
        path = Path(match.group(1) or match.group(2))
        if path.name not in compile_files or path.parent.name != "toolchain":
            return match.group(0)
        resolved = path.resolve()
        if not resolved.is_relative_to(build_root):
            raise ValueError(f"Compile response file outside build directory: {path}")
        return resolved.read_text(encoding="utf-8")

    restored = []
    for value in flags:
        if (isinstance(value, str) and value.startswith('@"')
                and any(value.endswith('/toolchain/' + name) for name in compile_files)
                and value.count('"') == 1):
            value += '"'
        if isinstance(value, str):
            value = re.sub(r'@"([^\"]+)"|@([^\s\"]+)', expand, value)
        restored.append(value)
    return Base.ParseFlags(self, *restored, **kwargs)


env.AddMethod(parse_flags_with_asm_response, "ParseFlags")
