"""Convert every downloaded character mod into one folder of MKDD-NX characters. Safe to stop and rerun:
it skips what its progress file says is done, and a mod that crashes or hangs is logged and passed over.
Usage: py -3 -I tools/charbatch.py WII_DIR GC_DIR STAGE_DIR OUT_DIR ISO CSV [CSV...]
Writes OUT_DIR/<id>/ per character, and OUT_DIR/_convert_all_log.csv with the result of each mod."""
import csv, faulthandler, json, os, re, shutil, subprocess, sys, tempfile, traceback

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
SEVENZ = r"C:\Program Files\7-Zip\7z.exe"
ARCHIVES = (".zip", ".rar", ".7z")
ITEM_SECONDS = 300  # a mod taking longer is treated as hung
RESTARTS = 300


def walk(d, exts):
    return [os.path.join(p, f) for p, _, fs in os.walk(d) for f in fs if f.lower().endswith(exts)]


def unpack(src, st):
    """src's archives, and archives inside those, into st (once; `-p-` so a password never prompts)"""
    marker = os.path.join(st, ".unpacked")
    if os.path.exists(marker):
        return
    os.makedirs(st, exist_ok=True)
    def x(a, to):
        try:
            subprocess.run([SEVENZ, "x", "-y", "-p-", "-o" + to, a], stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=ITEM_SECONDS)
        except subprocess.TimeoutExpired:
            pass
    for a in walk(src, ARCHIVES):
        x(a, os.path.join(st, os.path.splitext(os.path.basename(a))[0]))
    for a in walk(st, ARCHIVES):
        x(a, os.path.splitext(a)[0] + "_x")
    open(marker, "w").close()


def pick_drivers(files):
    """one driver file per folder and driver code: a kart's (standard kart first), not menus or allkart"""
    best = {}
    for f in files:
        name = os.path.basename(f).lower()
        if "allkart" in f.lower() or not re.search(r"_(kart|bike)[^-]*-[a-z]", name):
            continue
        pats = (r"^sdf_kart-", r"^m?df_kart-", r"_kart-", r"_bike-")
        rank = (next(i for i, p in enumerate(pats + (r"",)) if re.search(p, name)), bool(re.search(r"_\d\.szs$", name)), name)
        key = (os.path.dirname(f).lower(), name.rsplit("-", 1)[-1])
        if key not in best or rank < best[key][0]:
            best[key] = (rank, f)
    return sorted(f for _, f in best.values())


def worker(wii, gc, stage, out, iso, csvs):
    import charpack, wiichar
    faulthandler.enable()
    disc = charpack.Disc(iso)
    progress = os.path.join(out, "_convert_all.jsonl")
    done, started = {}, None
    if os.path.exists(progress):
        for line in open(progress, encoding="utf-8"):
            try:
                e = json.loads(line)
            except ValueError:
                continue
            if "start" in e:
                started = e["start"]
            else:
                done[e["key"]] = e
    pf = open(progress, "a", encoding="utf-8")
    def record(key, title, base, result, source=""):
        e = {"key": key, "title": title, "base": base, "result": result, "source": source}
        done[key] = e
        pf.write(json.dumps(e) + "\n"); pf.flush()
        print(f"{title}: {result}", flush=True)
    if started and started not in done:  # the last run died on it
        record(started, started, "", "FAIL: crashed or hung the converter")

    # what the earlier sample already converted
    old = os.path.join(out, "_convert_log.csv")
    old_ok = set()
    if os.path.exists(old):
        old_ok = {r["szs"] for r in csv.DictReader(open(old, encoding="utf-8")) if r["result"].startswith("ok")}
    ids = {d.lower() for d in os.listdir(out)}
    hashes = {}
    for d in os.listdir(out):
        p = os.path.join(out, d, "driver.bmd")
        if os.path.isfile(p):
            hashes[hash_bytes(open(p, "rb").read())] = d

    rows = {}
    for path in csvs:
        for r in csv.DictReader(open(path, encoding="utf-8")):
            rows.setdefault(re.sub(r'[<>:"/\\|?*]', "_", "%s - %s" % (r["character"], r["title"]))[:120], r)

    def new_id(title):
        cid = re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-") or "character"
        k, n = cid, 2
        while k in ids:
            k, n = f"{cid}-{n}", n + 1
        ids.add(k)
        return k

    def begin(key):
        faulthandler.dump_traceback_later(ITEM_SECONDS, exit=True)
        pf.write(json.dumps({"start": key}) + "\n"); pf.flush()

    # Wii: one character per distinct driver in the mod
    for folder in sorted(os.listdir(wii)):
        d = os.path.join(wii, folder)
        if not os.path.isdir(d):
            continue
        r = rows.get(folder, {})
        title = r.get("title") or folder.split(" - ", 1)[-1]
        key0 = "wii|" + folder
        if key0 in done:
            continue
        faulthandler.dump_traceback_later(ITEM_SECONDS * 4, exit=True)
        st = os.path.join(stage, "wii", folder)
        found = walk(d, (".szs",))
        if not found:
            old_st = os.path.join(os.path.dirname(wii), "wii_x", folder)  # the sample's unpacked copy
            if walk(old_st, (".szs",)):
                st = old_st
            else:
                unpack(d, st)
            found = walk(st, (".szs",))
        made = 0
        for szs in pick_drivers(found):
            key = "wii|" + os.path.relpath(szs, wii)
            if key in done:
                made += done[key]["result"].startswith("ok")
                continue
            if os.path.relpath(szs, wii) in old_ok:
                record(key, title, r.get("character", ""), "ok: already converted by the sample")
                made += 1
                continue
            begin(key)
            try:
                data = open(szs, "rb").read()
                hint = wiichar.guess_base(szs)
                base, why = wiichar.pick_base(data, disc, charpack.base_index(hint) if hint else None)
                if base is None:
                    raise wiichar.ConvertError("no Double Dash character fits: " + why)
                files, notes = wiichar.convert(data, base, disc)
                h = hash_bytes(files["driver.bmd"])
                if h in hashes:
                    record(key, title, r.get("character", ""), f"skip: same model as {hashes[h]}", szs)
                    continue
                pics, more = wiichar.icons(szs, files, charpack.yaz0(disc.driver_file(base, "driver.bmd") or b"") or None)
                files.update(pics)
                name = title if not made else f"{title} ({os.path.basename(os.path.dirname(szs))})"
                k = new_id(name)
                wiichar.write_character(os.path.join(out, k), files, name, base, r.get("author", ""), r.get("wiki", ""))
                hashes[h] = k
                made += 1
                warn = [t for lv, t in notes + more if lv in ("warn", "error")]
                record(key, name, r.get("character", ""), f"ok: {charpack.BASES[base][1]} as {k}" + (" | " + "; ".join(warn) if warn else ""), szs)
            except Exception as e:
                msg = str(e) if isinstance(e, wiichar.ConvertError) else repr(e)
                record(key, title, r.get("character", ""), "FAIL: " + msg, szs)
        record(key0, title, r.get("character", ""), f"mod done: {made} characters" if made else "skip: no usable driver .szs (see any lines above)")

    # GameCube: already MKDD models, through the Characters tool's own import
    for folder in sorted(os.listdir(gc)):
        d = os.path.join(gc, folder)
        key = "gc|" + folder
        if not os.path.isdir(d) or key in done:
            continue
        r = rows.get(folder, {})
        title = r.get("title") or folder.split(" - ", 1)[-1]
        begin(key)
        try:
            st = os.path.join(stage, "gc", folder)
            unpack(d, st)
            chars, bad = charpack.gather([st], disc)
            res = []
            with tempfile.TemporaryDirectory(dir=stage) as tmp:
                for c in chars:
                    c.author = c.author or r.get("author", "")
                    c.source = c.source or r.get("wiki", "")
                charpack.export(chars, tmp)
                for c, cid in zip(chars, charpack.folder_ids(chars)):
                    h = hash_bytes(open(os.path.join(tmp, cid, "driver.bmd"), "rb").read())
                    if h in hashes:
                        res.append(f"skip {c.name}: same model as {hashes[h]}")
                        continue
                    k = new_id(c.name or title)
                    shutil.move(os.path.join(tmp, cid), os.path.join(out, k))
                    hashes[h] = k
                    res.append(f"ok: {charpack.BASES[c.base][1]} as {k}")
            res += [f"REJECTED {w}: {y}" for w, y in bad]
            record(key, title, r.get("character", ""), "; ".join(res) or "skip: no character found")
        except Exception as e:
            record(key, title, r.get("character", ""), f"FAIL: {e!r} {traceback.format_exc(limit=-1).strip().splitlines()[-2]}")
    faulthandler.cancel_dump_traceback_later()

    with open(os.path.join(out, "_convert_all_log.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["item", "title", "wii base", "result"])
        for e in done.values():
            w.writerow([e["key"], e["title"], e["base"], e["result"]])
    rs = [e["result"] for e in done.values() if not e["key"].count("|") or "mod done" not in e["result"]]
    print("characters in folder:", sum(os.path.isfile(os.path.join(out, x, "character.ini")) for x in os.listdir(out)))
    print("failed:", sum(x.startswith("FAIL") for x in rs), " same model skipped:", sum("same model" in x for x in rs))


def hash_bytes(b):
    import hashlib
    return hashlib.md5(b).hexdigest()


def main(argv):
    if argv and argv[0] == "--worker":
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        worker(argv[1], argv[2], argv[3], argv[4], argv[5], argv[6:])
        return 0
    if len(argv) < 6:
        print(__doc__)
        return 2
    os.makedirs(argv[2], exist_ok=True)
    os.makedirs(argv[3], exist_ok=True)
    for _ in range(RESTARTS):  # the worker exits nonzero when a mod kills it; the next one logs and skips that mod
        if subprocess.call([sys.executable, "-I", os.path.abspath(__file__), "--worker"] + argv) == 0:
            return 0
    print("gave up after", RESTARTS, "restarts")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
