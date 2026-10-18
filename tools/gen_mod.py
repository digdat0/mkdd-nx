__doc__ = ("""Add a mod's code to the generated C: the functions whose C differs from the game's.

usage: python tools/gen_mod.py <game gen dir> <mod gen dir> <out dir> --name "<mod name>"

Both gen dirs come from tools/gen_c.py with the same flags: <game gen dir> from the game's
main.dol, <mod gen dir> from the mod's (Ghidra export of the mod's main.dol, the game's
functions.csv / hle.csv, meta.csv carrying the mod's SHA-1). A function is the mod's when its
generated C differs or is new; since the generator's whole-program analyses (register use of
callees, inlining) show up in the C, a caller the mod's changes affect differs too. <out dir>
gets the game's C with a check at the top of each such function, which hands over to the mod's
version (fp_<ADDR>) while a mod runs (g_mod), and part_mod.c with those versions and MOD_FUNCS
(the dispatcher's table for them). Everything else is shared: identical C, identical behaviour.

MKDD Plus 3.3 (NTSC) rewrites the immediate of 22 "li r0,N" instructions when it switches course
sets (its table at 0x803e2824 + 648, pointing at each instruction's low byte); generate its C with
  py -3 tools/gen_c.py <plus sheets> <mod gen dir> --dyn-imm=""" + ",".join(f"{a:#x}" for a in (
    0x801420d8, 0x80141ee0, 0x80141e14, 0x8014299c, 0x8014233c, 0x801421a4, 0x8014200c, 0x801424d4,
    0x80142a68, 0x80142408, 0x80142270, 0x80142738, 0x801425a0, 0x8014266c, 0x80142804, 0x801428d0,
    0x80142c14, 0x80142b6c, 0x80142ba4, 0x80142c4c, 0x80142b34, 0x80142bdc,
    0x80158ef0, 0x80158efc)) + """
(the last two are the port's: plus_class_order in runtime/misc.c swaps them to list 150cc before 200cc)
""")
import re
import shutil
import sys
from pathlib import Path

HEAD = re.compile(r"^(?:/\* [^\n]*\*/\n)?(ALWAYS_INLINE void fi_|void f_)([0-9A-F]{8})\(", re.M)
CALL = re.compile(r"\b(fi?)_([0-9A-F]{8})\b")


def functions(path):
    """{('f'|'fi', addr): C text} of one generated file."""
    text = path.read_text(encoding="utf-8")
    out, starts = {}, list(HEAD.finditer(text))
    for i, m in enumerate(starts):
        end = starts[i + 1].start() if i + 1 < len(starts) else len(text)
        out[("fi" if m[1].startswith("ALWAYS") else "f", int(m[2], 16))] = text[m.start():end].rstrip() + "\n"
    return out


def load(gen):
    funcs = {}
    for p in sorted(gen.glob("part_[0-9]*.c")) + [gen / "inl.h"]:
        funcs.update(functions(p))
    table = (gen / "table.c").read_text(encoding="utf-8")
    ranges = {int(a, 16): int(b, 16) for a, b in re.findall(r"\{0x([0-9a-f]{8})u, 0x([0-9a-f]{8})u, f_", table)}
    sha = re.search(r'DOL_SHA1\[\] = "([0-9a-f]*)"', table)[1]
    return funcs, ranges, sha


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--name=")]
    names = [a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--name=")]
    if len(args) != 3 or not names:
        sys.exit(__doc__)
    game_dir, mod_dir, out = (Path(a) for a in args)
    game, game_ranges, game_sha = load(game_dir)
    mod, mod_ranges, mod_sha = load(mod_dir)
    if game_sha == mod_sha:
        sys.exit("the two gen dirs come from the same main.dol")

    # the mod's functions: new or different C
    own = {a for (k, a), t in mod.items() if k == "f" and game.get(("f", a)) != t}
    inl = {a for (k, a), t in mod.items() if k == "fi" and game.get(("fi", a)) != t}
    # a caller of a changed inline body inlines the old one: it is the mod's too (in either version)
    for (k, a), t in list(mod.items()) + list(game.items()):
        if k == "f" and a in mod_ranges and any(c == "fi" and int(x, 16) in inl for c, x in CALL.findall(t)):
            own.add(a)
    gone = sorted(a for a in game_ranges if a not in mod_ranges)
    print(f"mod functions: {len(own)} ({sum(1 for a in own if a not in game_ranges)} new), changed inline bodies: {len(inl)}; "
          f"game functions the mod has none at: {len(gone)}")

    def rename(t):
        def sub(m):
            a = int(m[2], 16)
            if m[1] == "f" and a in own:
                return f"fp_{m[2]}"
            if m[1] == "fi" and a in inl:
                return f"fpi_{m[2]}"
            return m[0]
        return CALL.sub(sub, t)

    # out: the game's C, the mod's functions handing over
    out.mkdir(parents=True, exist_ok=True)
    for p in game_dir.iterdir():
        if p.is_file() and p.name != "part_mod.c":
            dst = out / p.name
            data = p.read_text(encoding="utf-8")
            if p.name == "funcs.h":  # the mod's versions, for the hand-over checks
                data += "".join(f"void fp_{a:08X}(Ctx *c,uint32_t entry);\n" for a in sorted(own))
            if p.name.startswith("part_"):
                data = re.sub(r"^void f_([0-9A-F]{8})\(Ctx \*c,uint32_t entry\)\{\n",
                              lambda m: m[0] + (f"if(UNLIKELY(g_mod)){{fp_{m[1]}(c,entry);return;}}\n" if int(m[1], 16) in own else ""),
                              data, flags=re.M)
            if not dst.exists() or dst.read_text(encoding="utf-8") != data:
                dst.write_text(data, encoding="utf-8", newline="\n")

    name = names[0].replace("\\", "\\\\").replace('"', '\\"')
    with open(out / "part_mod.c.tmp", "w", encoding="utf-8", newline="\n") as fh:
        fh.write(f"/* GENERATED by tools/gen_mod.py: the code of {name}. Do not edit. */\n"
                 "#include \"funcs.h\"\n#include \"inl.h\"\n#include \"table.h\"\n"
                 "#pragma GCC diagnostic ignored \"-Wunused-label\"\n#pragma GCC diagnostic ignored \"-Wunused-variable\"\n"
                 "#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"\n\n")
        for a in sorted(own):
            fh.write(f"void fp_{a:08X}(Ctx *c,uint32_t entry);\n")
        fh.write("\n")
        for a in sorted(inl):
            fh.write(rename(mod[("fi", a)]) + "\n")
        for a in sorted(own):
            fh.write(rename(mod[("f", a)]) + "\n")
        fh.write(f'const char MOD_DOL_SHA1[] = "{mod_sha}";\nconst char MOD_NAME[] = "{name}";\n')
        fh.write("const FuncEntry MOD_FUNCS[] = {\n")
        for a in sorted(own):
            fh.write(f"    {{0x{a:08x}u, 0x{mod_ranges[a]:08x}u, fp_{a:08X}}},\n")
        fh.write(f"}};\nconst unsigned MOD_FUNC_COUNT = {len(own)};\n")
    tmp = out / "part_mod.c.tmp"
    dst = out / "part_mod.c"
    if dst.exists() and dst.read_bytes() == tmp.read_bytes():
        tmp.unlink()
    else:
        shutil.move(tmp, dst)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
