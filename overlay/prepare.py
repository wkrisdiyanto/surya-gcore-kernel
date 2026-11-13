#!/usr/bin/env python3
"""Apply the gcore integration to a surya kernel tree."""
import sys
import pathlib

root = pathlib.Path(sys.argv[1]).resolve()


def patch(rel, old, new):
    p = root / rel
    s = p.read_text()
    if old not in s:
        raise SystemExit(f"anchor missing in {rel}")
    p.write_text(s.replace(old, new, 1))


patch(
    "fs/exec.c",
    "int search_binary_handler(struct linux_binprm *bprm)",
    "#ifdef CONFIG_GCORE\nvoid gcore_on_exec(const char *path);\n#endif\n\n"
    "int search_binary_handler(struct linux_binprm *bprm)",
)

patch(
    "fs/exec.c",
    "\tretval = security_bprm_check(bprm);\n\tif (retval)\n\t\treturn retval;\n",
    "\tretval = security_bprm_check(bprm);\n\tif (retval)\n\t\treturn retval;\n"
    "\n#ifdef CONFIG_GCORE\n\tgcore_on_exec(bprm->filename);\n#endif\n",
)

p = root / "drivers/Kconfig"
s = p.read_text()
i = s.rfind("endmenu")
if i < 0:
    raise SystemExit("endmenu missing in drivers/Kconfig")
p.write_text(s[:i] + 'source "drivers/gcore/Kconfig"\n' + s[i:])

p = root / "drivers/Makefile"
s = p.read_text()
if not s.endswith("\n"):
    s += "\n"
p.write_text(s + "\nobj-$(CONFIG_GCORE) += gcore/\n")

p = root / "arch/arm64/configs/surya_defconfig"
s = p.read_text()
if "CONFIG_GCORE" not in s:
    if not s.endswith("\n"):
        s += "\n"
    s += "CONFIG_GCORE=y\n"
p.write_text(s)

print("gcore overlay applied")
