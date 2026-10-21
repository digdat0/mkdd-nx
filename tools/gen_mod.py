PLUS_KEEP = (0x80164e7c, 0x80178fac)
RTGP_KEEP = (0x80154d18, 0x8016b27c, 0x80172300, 0x8017230c, 0x80172318, 0x80172340, 0x80179298, 0x8017c7d4,
             0x80186648, 0x80187e24, 0x801e5dc4)
__doc__ = ("""Add mods' code to the generated C: for each mod, the functions whose C differs from the game's.

usage: python tools/gen_mod.py <game gen dir> <out dir> --mod=<mod gen dir>,<key>,<mod name> [--mod=...]

All gen dirs come from tools/gen_c.py with the same flags: <game gen dir> from the game's
main.dol, each <mod gen dir> from that mod's (Ghidra export of the mod's main.dol, the game's
functions.csv / hle.csv, meta.csv carrying the mod's SHA-1). A function is a mod's when its
generated C differs or is new; since the generator's whole-program analyses (register use of
callees, inlining) show up in the C, a caller the mod's changes affect differs too. <out dir>
gets the game's C with a check at the top of each such function, which hands over to mod n's
version (fp<n>_<ADDR>) while mod n runs (g_mod == n), part_mod<n>.c with mod n's versions and
its table for the dispatcher, and part_mod.c with MODS (one entry per mod, in --mod order: key,
name, SHA-1, functions). <key> is the mod's folder in the app's mods/ and names its saves.
Everything else is shared: identical C, identical behaviour.

MKDD Plus 3.3 (NTSC) rewrites the immediate of 22 "li r0,N" instructions when it switches course
sets (its table at 0x803e2824 + 648, pointing at each instruction's low byte); generate its C with
  py -3 tools/gen_c.py <plus sheets> <mod gen dir> --dyn-imm=""" + ",".join(f"{a:#x}" for a in (
    0x801420d8, 0x80141ee0, 0x80141e14, 0x8014299c, 0x8014233c, 0x801421a4, 0x8014200c, 0x801424d4,
    0x80142a68, 0x80142408, 0x80142270, 0x80142738, 0x801425a0, 0x8014266c, 0x80142804, 0x801428d0,
    0x80142c14, 0x80142b6c, 0x80142ba4, 0x80142c4c, 0x80142b34, 0x80142bdc,
    0x80158ef0, 0x80158efc)) + """
(the last two are the port's: plus_class_order in runtime/misc.c swaps them to list 150cc before 200cc)
and --keep-regs=""" + ",".join(f"{a:#x}" for a in PLUS_KEEP) + """

Mods hook their code in with a "bl" where the game had another instruction, and the game code
around it keeps values in volatile registers across that call (the hook leaves them alone): give
gen_c.py those call sites (the "bl" words of the mod's main.dol that differ from the game's and
replaced something other than a "bl") as --keep-regs, or the C loses those values (Retro Track
Grand Prix 2.1 hung loading a course: a loop count in r0). The hooks themselves hand back every
register they set, non-volatile ones too (RTGP's do the replaced "mr r0,r3" / "mr r31,r3").
Retro Track Grand Prix 2.1:
  py -3 tools/gen_c.py <rtgp sheets> <mod gen dir> --keep-regs=""" + ",".join(f"{a:#x}" for a in RTGP_KEEP) + """
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


def c_str(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def write_if_changed(dst, text):
    tmp = dst.with_name(dst.name + ".tmp")
    tmp.write_text(text, encoding="utf-8", newline="\n")
    if dst.exists() and dst.read_bytes() == tmp.read_bytes():
        tmp.unlink()
    else:
        shutil.move(tmp, dst)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--mod=")]
    specs = [a.split("=", 1)[1].split(",", 2) for a in sys.argv[1:] if a.startswith("--mod=")]
    if len(args) != 2 or not specs or any(len(s) != 3 or not re.fullmatch(r"[a-z0-9_-]+", s[1]) for s in specs):
        sys.exit(__doc__)
    game_dir, out = (Path(a) for a in args)
    game, game_ranges, game_sha = load(game_dir)

    mods = []  # per mod: key, name, sha, own, inl, functions, ranges
    for n, (mod_dir, key, name) in enumerate(specs, 1):
        mod, mod_ranges, mod_sha = load(Path(mod_dir))
        if game_sha == mod_sha:
            sys.exit(f"{mod_dir} comes from the game's main.dol")
        # the mod's functions: new or different C
        own = {a for (k, a), t in mod.items() if k == "f" and game.get(("f", a)) != t}
        inl = {a for (k, a), t in mod.items() if k == "fi" and game.get(("fi", a)) != t}
        # a caller of a changed inline body inlines the old one: it is the mod's too (in either version)
        for (k, a), t in list(mod.items()) + list(game.items()):
            if k == "f" and a in mod_ranges and any(c == "fi" and int(x, 16) in inl for c, x in CALL.findall(t)):
                own.add(a)
        gone = sorted(a for a in game_ranges if a not in mod_ranges)
        print(f"mod {n} ({name}): {len(own)} functions ({sum(1 for a in own if a not in game_ranges)} new), "
              f"changed inline bodies: {len(inl)}; game functions the mod has none at: {len(gone)}")
        mods.append((key, name, mod_sha, own, inl, mod, mod_ranges))

    # out: the game's C, each mod's functions handing over
    out.mkdir(parents=True, exist_ok=True)
    for p in game_dir.iterdir():
        if p.is_file() and not p.name.startswith("part_mod"):
            data = p.read_text(encoding="utf-8")
            if p.name == "funcs.h":  # the mods' versions, for the hand-over checks
                data += "".join(f"void fp{n}_{a:08X}(Ctx *c,uint32_t entry);\n"
                                for n, m in enumerate(mods, 1) for a in sorted(m[3]))
            if p.name.startswith("part_"):
                def check(m):
                    a = int(m[1], 16)
                    return m[0] + "".join(f"if(UNLIKELY(g_mod=={n})){{fp{n}_{m[1]}(c,entry);return;}}\n"
                                          for n, mod in enumerate(mods, 1) if a in mod[3])
                data = re.sub(r"^void f_([0-9A-F]{8})\(Ctx \*c,uint32_t entry\)\{\n", check, data, flags=re.M)
            write_if_changed(out / p.name, data)

    for n, (key, name, mod_sha, own, inl, mod, mod_ranges) in enumerate(mods, 1):
        def rename(t):
            def sub(m):
                a = int(m[2], 16)
                if m[1] == "f" and a in own:
                    return f"fp{n}_{m[2]}"
                if m[1] == "fi" and a in inl:
                    return f"fpi{n}_{m[2]}"
                return m[0]
            return CALL.sub(sub, t)
        text = (f"/* GENERATED by tools/gen_mod.py: the code of {c_str(name)}. Do not edit. */\n"
                "#include \"funcs.h\"\n#include \"inl.h\"\n#include \"table.h\"\n"
                "#pragma GCC diagnostic ignored \"-Wunused-label\"\n#pragma GCC diagnostic ignored \"-Wunused-variable\"\n"
                "#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"\n\n")
        text += "".join(f"void fp{n}_{a:08X}(Ctx *c,uint32_t entry);\n" for a in sorted(own)) + "\n"
        text += "".join(rename(mod[("fi", a)]) + "\n" for a in sorted(inl))
        text += "".join(rename(mod[("f", a)]) + "\n" for a in sorted(own))
        text += f"const FuncEntry MOD{n}_FUNCS[] = {{\n"
        text += "".join(f"    {{0x{a:08x}u, 0x{mod_ranges[a]:08x}u, fp{n}_{a:08X}}},\n" for a in sorted(own))
        text += "};\n"
        write_if_changed(out / f"part_mod{n}.c", text)
    for p in out.glob("part_mod*.c"):  # a mod an earlier run had and this one hasn't
        m = re.fullmatch(r"part_mod(\d+)\.c", p.name)
        if m and not 1 <= int(m[1]) <= len(mods):
            p.unlink()

    text = "/* GENERATED by tools/gen_mod.py: the mods this build carries. Do not edit. */\n#include \"table.h\"\n"
    text += "".join(f"extern const FuncEntry MOD{n}_FUNCS[];\n" for n in range(1, len(mods) + 1))
    text += "const ModInfo MODS[] = {\n"
    text += "".join(f'    {{"{key}", "{c_str(name)}", "{sha}", MOD{n}_FUNCS, {len(own)}}},\n'
                    for n, (key, name, sha, own, *_) in enumerate(mods, 1))
    text += f"}};\nconst unsigned MOD_COUNT = {len(mods)};\n"
    write_if_changed(out / "part_mod.c", text)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
