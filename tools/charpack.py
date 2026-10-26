#!/usr/bin/env python3
"""MKDD-NX character pack tool: turn downloaded character mods into MKDD-NX character folders.

Each character becomes one folder, copied to the SD card's /switch/MKDD-NX/mods/characters/:

    <id>/character.ini   name, base, author, source; slot (optional: the cell it shows in)
    <id>/driver.bmd      the model (required); driver_200.bmd, *.btp face animations
    <id>/face.bti        HUD and select screen face   (optional)
    <id>/map.bti         map icon        (optional: the game makes one from the face)
    <id>/name.bti        select screen name tag (optional: the game writes the name)
    <id>/anims/          its own driving animations (the packs' are another character's,
                         Mario's, copied: the game plays that character's)
    <id>/voice/          its own voice (optional): <archive>/<wave>.wav (Voice/, SelectVoice/,
                         CommendationVoice/), played in place of its base's

Input: a mod folder as shipped for the MKDD Patcher (modinfo.ini + files/MRAM.arc/mram/driver/
<character>/...), any number of characters, a folder made by this tool, or a folder with just one
loose model (its character found by its skeleton, with the disc image: pick it under "On"
otherwise); or a folder holding such folders (each one added). In the window, folders can also be dropped on it. A mod the game
can't use (no model, a broken one, a skeleton no character has) or one already in the list (the
same driver.bmd) is rejected, with the reason. A model with another character's skeleton (Mario's
on Bowser Jr.) moves with that character's animations in the game.

Where each character shows (the select screen's cell and page) follows the game's placement
(runtime/charpack.c layout): its own character's cell up to an even share of the pages, the rest
in the emptiest cells; slot= pins one to a cell. With the disc image
given, each model is checked against the character it replaces: the skeleton (joints; another
character's: it moves as that one) and the textures (the select screen shows the variant's textures on the
game's model). Yaz0-compressed files are decompressed.

A one-character mod's sounds become its voice/: its audio_waves/ (the MKDD Patcher's .wav files)
or, in older mods, its rebuilt AudioRes/ (GCKart.baa and Waves/*.aw: the waves that differ from the
disc's, decoded). With the disc image, each is made what the game expects in place of the wave it
replaces, as the MKDD Patcher and MKDD Plus do: mono, no higher sample rate, no longer than the
original (the game's sound tables are made for those lengths); the game cuts long ones too.

    py -3 tools/charpack.py                     the window
    py -3 tools/charpack.py --cli --iso GAME.iso --out OUT MOD [MOD ...]   check and export

The disc image is only read, to compare; nothing from it is written.
"""
import array
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import types

# ---- characters, in MKDD-NX's order (the select screen's grid) ----
# id (character.ini base=), name, game driver folder, race2d face, map icon, menu name tag
BASES = [
    ("mario", "Mario", "mario", "mario", "Mario", "mario"),
    ("luigi", "Luigi", "luigi", "luigi", "Luigi", "luigi"),
    ("peach", "Peach", "peach", "peach", "Peach", "peach"),
    ("daisy", "Daisy", "daisy", "daisy", "Daisy", "daisy"),
    ("yoshi", "Yoshi", "yoshi", "yoshi", "yoshi", "yoshi"),
    ("birdo", "Birdo", "catherine", "catherine", "Catherine", "birdo"),
    ("baby_mario", "Baby Mario", "babymario", "baby_mario", "BabyMario", "baby_mario"),
    ("baby_luigi", "Baby Luigi", "babyluigi", "baby_luigi", "BabyLuigi", "baby_luigi"),
    ("toad", "Toad", "kinopio", "kinopio", "Kinopio", "kinopio"),
    ("toadette", "Toadette", "kinopico", "kinopico", "Kinopico", "kinopico"),
    ("koopa", "Koopa", "nokonoko", "noko", "Nokonoko", "koopa"),
    ("paratroopa", "Paratroopa", "patapata", "pata", "Patapata", "paratoopa"),
    ("donkey_kong", "Donkey Kong", "dk", "donky", "DonkeyKong", "donkey_kong"),
    ("diddy_kong", "Diddy Kong", "dkjr", "diddy", "DiddyKong", "diddy_kong"),
    ("bowser", "Bowser", "koopa", "koopa", "Koopa", "bowser"),
    ("bowser_jr", "Bowser Jr.", "koopajr", "koopa_jr", "Koopajr", "bowser_jr"),
    ("wario", "Wario", "wario", "wario", "Wario", "wario"),
    ("waluigi", "Waluigi", "waluigi", "waluigi", "Waluigi", "waluigi"),
    ("petey", "Petey Piranha", "bosspakkun", "pakkun", "Pakkun", "pakkun"),
    ("king_boo", "King Boo", "kingteresa", "teresa", "Teresa", "teresa"),
]
BASE_IDS = [b[0] for b in BASES]
# Crow8028's Character Texture Pack has no names of its own
CROW_NAMES = ["Metal Mario", "Gooigi", "Bride Peach", "HoliDaisy", "Blue Yoshi", "Yellow Birdo", "Diaper Baby Mario",
              "Baby Waluigi", "Blue Toad", "Purple Toadette", "Gold Koopa", "Blue Paratroopa", "White DK",
              "Black Diddy Kong", "Dark Lava Bowser", "Shadow Mario", "Bull Wario", "Bus Driver Waluigi",
              "Putrid Petey", "Pink Boo"]
# packs naming their characters only on their name tags: modinfo.ini's modname -> driver folder -> name
PACK_NAMES = {"puyo pack": {"mario": "Arle", "luigi": "Ringo", "yoshi": "Sig", "catherine": "Amitie", "dk": "Feli",
                            "dkjr": "Lemres", "koopa": "Satan", "koopajr": "Draco"}}
DRIVERS = ("MRAM.arc", "mram", "driver")
KARTS = ("MRAM.arc", "mram", "kart")
ANIMS = ("ARAM.arc", "aram", "driveranm")
FACES = ("race2d.arc", "mram_race2d", "timg")
NAMES = ("SceneData", "English", "menu.arc", "menu", "timg")


def base_of_folder(d):
    """the game's driver folder name (MKDD Patcher layout) -> 0..19 (None): "koopa" is Bowser here"""
    for i, b in enumerate(BASES):
        if d.lower() == b[2]:
            return i
    return None


def base_index(s):
    """base= value, a character's name, or the game's driver folder -> 0..19 (None)"""
    s = s.strip().lower()
    for i, b in enumerate(BASES):
        if s in (b[0], b[1].lower()):
            return i
    for i, b in enumerate(BASES):
        if s == b[2]:
            return i
    return None


# ---- files ----
def find_ci(folder, *parts):
    """folder/parts..., matching each part regardless of case (SD cards and Windows don't care,
    mods are written either way); None when missing"""
    p = folder
    for part in parts:
        if not os.path.isdir(p):
            return None
        hit = [n for n in os.listdir(p) if n.lower() == part.lower()]
        if not hit:
            return None
        p = os.path.join(p, hit[0])
    return p


def yaz0(b):
    """Yaz0-decompressed bytes (the bytes as they are when not Yaz0)"""
    if b[:4] != b"Yaz0":
        return b
    size = struct.unpack(">I", b[4:8])[0]
    out = bytearray()
    i = 16
    while len(out) < size:
        code = b[i]
        i += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> bit):
                out.append(b[i])
                i += 1
            else:
                b1, b2 = b[i], b[i + 1]
                i += 2
                dist = ((b1 & 0x0F) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = b[i] + 0x12
                    i += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)


def read(path):
    with open(path, "rb") as f:
        return yaz0(f.read())


# ---- voices: audio_waves/<archive>/<wave id>.wav (MKDD Patcher), the character folder's voice/ ----
# the Patcher's folder names: the .aw each replaces waves of, and where the game plays it
VOICE_ARCS = {"selectvoice": ("SelectVoice", "SelectVoice_0.aw", "select screen"),
              "voice": ("Voice", "Voice_0.aw", "race"),
              "commendationvoice": ("CommendationVoice", "CommendationVoice_0.aw", "awards"),
              "soundeffects": ("SoundEffects", "se00_0.aw", "sound effects"),
              "nintendologo": ("NintendoLogo", "NintendoLogoMario_0.aw", "Nintendo logo"),
              "bgmsamples": ("BGMSamples", "bgm_0.aw", "music")}


def wav_info(path):
    """(rate, samples) of a .wav the game plays (16- or 8-bit PCM, any channels: runtime/charvoice.c),
    else the reason it can't"""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        return "not a .wav"
    fmt = data = None
    o = 12
    while o + 8 <= len(d):
        tag, n = d[o:o + 4], struct.unpack("<I", d[o + 4:o + 8])[0]
        if tag == b"fmt " and n >= 16:
            fmt = struct.unpack("<HHIIHH", d[o + 8:o + 24])
            if fmt[0] == 0xFFFE and n >= 26:  # WAVE_FORMAT_EXTENSIBLE: its subformat
                fmt = (struct.unpack("<H", d[o + 32:o + 34])[0],) + fmt[1:]
        elif tag == b"data":
            data = min(n, len(d) - o - 8)
        o += 8 + n + (n & 1)
    if not fmt or data is None:
        return "not a .wav (no fmt or data)"
    kind, ch, rate, _, _, bits = fmt
    if kind != 1 or bits not in (8, 16) or not 1 <= ch <= 8:
        return f"{bits}-bit {'PCM' if kind == 1 else f'format {kind}'}: the game plays 16- or 8-bit PCM"
    if not 1000 <= rate <= 192000 or not data:
        return "empty, or an odd sample rate"
    return rate, data // (ch * bits // 8)


def read_wav(path):
    """(rate, mono int16 samples) of a .wav, the channels averaged"""
    with open(path, "rb") as f:
        d = f.read()
    o, fmt, data = 12, None, b""
    while o + 8 <= len(d):
        tag, n = d[o:o + 4], struct.unpack("<I", d[o + 4:o + 8])[0]
        if tag == b"fmt ":
            fmt = struct.unpack("<HHIIHH", d[o + 8:o + 24])
        elif tag == b"data":
            data = d[o + 8:o + 8 + n]
        o += 8 + n + (n & 1)
    ch, rate, bits = fmt[1], fmt[2], fmt[5]
    width = bits // 8
    frames = len(data) // (ch * width)
    if bits == 16:
        v = array.array("h", data[:frames * ch * 2])
        if sys.byteorder == "big":
            v.byteswap()
    else:
        v = array.array("h", ((b - 128) << 8 for b in data[:frames * ch]))
    if ch > 1:
        v = array.array("h", (sum(v[k * ch:k * ch + ch]) // ch for k in range(frames)))
    return rate, v


def write_wav(path, rate, v):
    v = array.array("h", v)
    if sys.byteorder == "big":
        v.byteswap()
    data = v.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
                struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(data)) + data)


def conform(rate, v, orig_rate, orig_count):
    """a sound as the game gets it in place of a wave of orig_count samples at orig_rate: no faster
    than that rate (linear resampling) and no longer than that wave (the MKDD Patcher's and MKDD
    Plus's rules: the game's sound tables are made for the original lengths)"""
    orig_rate = int(round(orig_rate))
    if rate > orig_rate:
        step, n = rate / orig_rate, int(len(v) * orig_rate / rate)
        out = array.array("h", bytes(2 * n))
        for k in range(n):
            x = k * step
            a = int(x)
            b = min(a + 1, len(v) - 1)
            out[k] = int(v[a] + (v[b] - v[a]) * (x - a))
        rate, v = orig_rate, out
    most = int(orig_count * rate / orig_rate)
    return rate, v[:most]


AFC_COEF = ((0, 0), (2048, 0), (0, 2048), (1024, 1024), (4096, -2048), (3584, -1536), (3072, -1024), (4608, -2560),
            (4200, -2248), (4800, -2300), (5120, -3072), (2048, -2048), (1024, -1024), (-1024, 1024), (-1024, 0), (-2048, 0))


def decode_wave(d, fmt, count):
    """a wave of an .aw as int16 samples: format 0 4-bit AFC ADPCM (9 bytes a 16 samples), 1 2-bit
    (5 bytes), 2 PCM8, 3 PCM16 (as runtime/dsp.c plays them)"""
    if fmt == 3:
        v = array.array("h", d[:2 * count])
        if sys.byteorder == "little":
            v.byteswap()
        return v
    if fmt == 2:
        return array.array("h", ((b - 256 if b > 127 else b) << 8 for b in d[:count]))
    size, out, h0, h1 = (9 if fmt == 0 else 5), array.array("h"), 0, 0
    for blk in range(0, len(d) - size + 1, size):
        hd = d[blk]
        delta, (c0, c1) = 1 << (hd >> 4), AFC_COEF[hd & 15]
        for k in range(16):
            if size == 9:
                nib = d[blk + 1 + k // 2] >> (0 if k & 1 else 4) & 15
                val, shift = nib - 16 if nib > 7 else nib, 11
            else:
                bits = d[blk + 1 + k // 4] >> (6 - 2 * (k % 4)) & 3
                val, shift = bits - 4 if bits > 1 else bits, 13
            x = ((delta * val) << shift) + h0 * c0 + h1 * c1 >> 11
            x = max(-32768, min(32767, x))
            h1, h0 = h0, x
            out.append(x)
        if len(out) >= count:
            break
    return out[:count]


def wave_tables(baa):
    """{.aw name (lower case): {wave id: (format, rate, offset, bytes, samples)}} of a GCKart.baa"""
    out = {}
    u32 = lambda o: struct.unpack(">I", baa[o:o + 4])[0]
    for w in range(0, len(baa) - 0x18, 4):
        if baa[w:w + 4] != b"WSYS":
            continue
        try:
            winf, wbct = w + u32(w + 0x10), w + u32(w + 0x14)
            if baa[winf:winf + 4] != b"WINF" or baa[wbct:wbct + 4] != b"WBCT":
                continue
            for g in range(min(u32(winf + 4), u32(wbct + 8))):
                go, so = w + u32(winf + 8 + 4 * g), w + u32(wbct + 12 + 4 * g)
                aw = baa[go:go + 0x70].split(b"\0")[0].decode("latin-1").lower()
                cdf, table = w + u32(so + 0xc), {}
                for i in range(u32(go + 0x70)):
                    wo, eo = w + u32(go + 0x74 + 4 * i), w + u32(cdf + 8 + 4 * i)
                    rate = struct.unpack(">f", baa[wo + 4:wo + 8])[0]
                    table.setdefault(struct.unpack(">h", baa[eo + 2:eo + 4])[0],
                                     (baa[wo + 1], rate, u32(wo + 8), u32(wo + 0xc), u32(wo + 0x1c)))
                out.setdefault(aw, table)
        except (struct.error, IndexError):
            continue
    return out


def voice_files(folder):
    """the sounds in an audio_waves (or voice) folder: ([(archive dir, wave id, path, (rate, samples))],
    [(file, why)])"""
    ok, bad = [], []
    for sub in sorted(os.listdir(folder)):
        sp = os.path.join(folder, sub)
        if not os.path.isdir(sp):
            bad.append((sub, "not in an archive folder (Voice/, SelectVoice/, ...)"))
            continue
        arc = VOICE_ARCS.get(sub.lower())
        for name in sorted(os.listdir(sp)):
            rel = f"{sub}/{name}"
            if arc is None:
                bad.append((rel, f"unknown folder {sub} (known: {', '.join(a[0] for a in VOICE_ARCS.values())})"))
            elif not re.fullmatch(r"\d+\.wav", name, re.I):
                bad.append((rel, "not named <wave number>.wav"))
            else:
                info = wav_info(os.path.join(sp, name))
                if isinstance(info, str):
                    bad.append((rel, info))
                else:
                    ok.append((arc[0], int(name[:-4]), os.path.join(sp, name), info))
    return ok, bad


def archive_sounds(audiores, disc):
    """the waves a mod's own AudioRes (GCKart.baa and Waves/*.aw, rebuilt whole: older mods, Sonic)
    has changed from the disc's: ([(archive dir, wave id, rate, samples)], [problems])"""
    baa = find_ci(audiores, "GCKart.baa")
    with open(baa, "rb") as f:
        mine = wave_tables(f.read())
    theirs = disc.wave_tables()
    out, bad = [], []
    for arc_dir, aw, _ in VOICE_ARCS.values():
        path = find_ci(audiores, "Waves", aw)
        if not path or aw.lower() not in mine:
            continue
        with open(path, "rb") as f:
            data = f.read()
        orig = disc.file("AudioRes/Waves/" + aw)
        for wid, (fmt, rate, off, size, count) in sorted(mine[aw.lower()].items()):
            o = theirs.get(aw.lower(), {}).get(wid)
            if o is None:
                bad.append(f"{arc_dir}/{wid}: no such wave in the game")
            elif data[off:off + size] != orig[o[2]:o[2] + o[3]] or abs(rate - o[1]) > 0.5:
                if off + size > len(data) or fmt > 3:
                    bad.append(f"{arc_dir}/{wid}: {'format ' + str(fmt) if fmt > 3 else 'cut short'}")
                else:
                    out.append((arc_dir, wid, int(round(rate)), decode_wave(data[off:off + size], fmt, count)))
    return out, bad


def voice_check(c, disc, n):
    """c.voice: [(archive dir, wave id, rate, samples)] the export writes, conformed with the disc"""
    c.voice = []
    sounds, cut, unknown = [], [], []
    if "voice" in c.files:
        ok, bad = voice_files(c.files["voice"])
        for rel, why in bad:
            n.append(("warn", f"voice: {rel} skipped: {why}"))
        sounds = [(arc, wid, *read_wav(path)) for arc, wid, path, _ in ok]
    if "audiores" in c.files:
        if not disc:
            n.append(("warn", "voice: the mod's sounds are in rebuilt sound archives (AudioRes): pick the disc image "
                              "to find the ones it changes"))
        else:
            sounds, bad = archive_sounds(c.files["audiores"], disc)
            for why in bad:
                n.append(("warn", f"voice: {why}, skipped"))
    if not sounds:
        if "voice" in c.files or (disc and "audiores" in c.files):
            n.append(("warn", "voice: none of its sounds can be played: it has its base's voice"))
        return
    for arc, wid, rate, v in sounds:
        if disc:
            orig = disc.waves(arc).get(wid)
            if orig is None:
                unknown.append(f"{arc}/{wid}")
                continue
            r2, v2 = conform(rate, v, orig[0], orig[1])
            if len(v2) < len(v) * r2 / rate - 1:
                cut.append(f"{arc}/{wid} {len(v) / rate:.2f} s to {orig[1] / orig[0]:.2f} s")
            rate, v = r2, v2
        c.voice.append((arc, wid, rate, v))
    if unknown:
        n.append(("warn", f"voice: no such wave in the game, skipped: {', '.join(unknown)}"))
    per = {}
    for arc, *_ in c.voice:
        per[arc] = per.get(arc, 0) + 1
    where = ", ".join(f"{k} {v} ({next(a[2] for a in VOICE_ARCS.values() if a[0] == k)})" for k, v in per.items())
    n.append(("ok", f"voice: {len(c.voice)} sounds of its own, played in place of {BASES[c.base][1]}'s: {where}"))
    if cut:
        n.append(("info", f"voice: {len(cut)} longer than the sounds they replace, cut to their length (as the MKDD "
                          f"Patcher does): {'; '.join(cut[:4])}{' ...' if len(cut) > 4 else ''}"))
    if not disc:
        n.append(("info", "voice: no disc image given: wave numbers and lengths not checked (the game cuts long ones)"))

# ---- J3D models: the joint and texture names ----
def _names(b, at):
    n = struct.unpack(">H", b[at:at + 2])[0]
    out = []
    for i in range(n):
        o = at + struct.unpack(">H", b[at + 4 + 4 * i + 2:at + 4 + 4 * i + 4])[0]
        out.append(b[o:b.index(b"\0", o)].decode("latin-1"))
    return out


def bmd_info(b):
    """{'joints': [...], 'textures': [...]} of a .bmd, or None"""
    if b[:7] != b"J3D2bmd":
        return None
    info = {"joints": [], "textures": []}
    o = 0x20
    while o + 8 <= len(b):
        magic, size = b[o:o + 4], struct.unpack(">I", b[o + 4:o + 8])[0]
        if size < 8:
            break
        if magic == b"JNT1":
            info["joints"] = _names(b, o + struct.unpack(">I", b[o + 0x14:o + 0x18])[0])
        elif magic == b"TEX1":
            info["textures"] = _names(b, o + struct.unpack(">I", b[o + 0x10:o + 0x14])[0])
        o += size
    return info


# ---- the disc image (only read): GameCube FST and RARC archives ----
class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")
        if self._read(0, 6) != b"GM4E01":
            raise ValueError("not Mario Kart: Double Dash!! USA (GM4E01)")
        fo, fs = struct.unpack(">II", self._read(0x424, 8))
        fst = self._read(fo, fs)
        n = struct.unpack(">I", fst[8:12])[0]
        strs = n * 12
        self.files = {}
        # the FST's paths: directories end where their next-entry index says
        ends, stack = [], []
        for i in range(1, n):
            while ends and i >= ends[-1]:
                ends.pop()
                stack.pop()
            e = fst[i * 12:i * 12 + 12]
            o = strs + (struct.unpack(">I", e[0:4])[0] & 0xFFFFFF)
            name = fst[o:fst.index(b"\0", o)].decode("latin-1")
            if e[0]:
                stack.append(name)
                ends.append(struct.unpack(">I", e[8:12])[0])
            else:
                self.files["/".join(stack + [name]).lower()] = struct.unpack(">II", e[4:12])
        self._mram = None

    def _read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def file(self, path):
        off, size = self.files[path.lower()]
        return self._read(off, size)

    def joints(self, base):
        """the joint names of the game's driver.bmd of base (cached)"""
        if not hasattr(self, "_joints"):
            self._joints = {}
        if base not in self._joints:
            b = self.driver_file(base, "driver.bmd")
            info = bmd_info(yaz0(b)) if b else None
            self._joints[base] = info["joints"] if info else []
        return self._joints[base]

    def wave_tables(self):
        """GCKart.baa's wave tables (wave_tables), read once"""
        if not hasattr(self, "_waves"):
            self._waves = wave_tables(self.file("AudioRes/GCKart.baa"))
        return self._waves

    def waves(self, arc):
        """{wave id: (sample rate, samples)} of a Patcher archive folder name (Voice)"""
        aw = next(a[1] for a in VOICE_ARCS.values() if a[0] == arc).lower()
        return {k: (v[1], v[4]) for k, v in self.wave_tables().get(aw, {}).items()}

    def driver_file(self, base, name):
        """the game's MRAM.arc Driver/<folder>/<name>, or None"""
        if self._mram is None:
            self._mram = rarc(yaz0(self.file("MRAM.arc")))
        return self._mram.get((BASES[base][2], name.lower()))


def rarc(d):
    """{(directory name, file name): bytes}, both lower case"""
    out = {}
    if d[:4] != b"RARC":
        return out
    nn, no, ne, eo, ss, so = struct.unpack(">IIIIII", d[0x20:0x38])
    no, eo, so = no + 0x20, eo + 0x20, so + 0x20
    data = 0x20 + struct.unpack(">I", d[0x0C:0x10])[0]

    def s(o):
        return d[so + o:d.index(b"\0", so + o)].decode("latin-1").lower()

    for k in range(nn):
        node = d[no + 16 * k:no + 16 * k + 16]
        dname = s(struct.unpack(">I", node[4:8])[0])
        cnt, first = struct.unpack(">H", node[10:12])[0], struct.unpack(">I", node[12:16])[0]
        for j in range(first, first + cnt):
            e = d[eo + 20 * j:eo + 20 * j + 20]
            if not (e[4] & 1):
                continue
            off, size = struct.unpack(">II", e[8:16])
            out[(dname, s(struct.unpack(">H", e[6:8])[0]))] = d[data + off:data + off + size]
    return out


# ---- characters found in a mod folder ----
class Character:
    def __init__(self, base, name, author="", source="", files=None, src="", slot=None):
        self.base, self.name, self.author, self.source = base, name, author, source
        self.slot = slot          # the cell it is pinned to (character.ini slot=), None: placed by the game
        self.files = files or {}  # name in the character folder -> source path (or a folder for anims/, voice/)
        self.src = src            # where it came from, for the list
        self.notes = []           # (level, text): level "ok", "info", "warn", "error"
        self.loose = False        # a loose model: its base from its skeleton (gather), else chosen
        self.unknown = False      # ... and none was found

    @property
    def id(self):
        return re.sub(r"[^a-z0-9]+", "-", self.name.lower()).strip("-") or BASE_IDS[self.base]


def ini_read(path):
    out = {}
    if path and os.path.isfile(path):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if "=" in line and not line.lstrip().startswith(("#", ";", "[")):
                    k, v = line.split("=", 1)
                    out[k.strip().lower()] = v.strip()
    return out


def unwrap(folder):
    """a zip's own top folder (or two): down to where files/ or character.ini is"""
    for _ in range(3):
        if find_ci(folder, "character.ini") or find_ci(folder, "files"):
            break
        subs = [d for d in os.listdir(folder) if os.path.isdir(os.path.join(folder, d))]
        if len(subs) != 1:
            break
        folder = os.path.join(folder, subs[0])
    return folder


def not_a_mod(folder):
    """why a folder without the Patcher layout can't be used, in words"""
    names = [f.lower() for _, _, fs in os.walk(folder) for f in fs]
    if names and sum(n.startswith("tex1_") and n.endswith((".png", ".dds")) for n in names) * 2 > len(names):
        return "a Dolphin texture pack (tex1_*.png), not a model mod: it replaces textures by hash and has no driver.bmd"
    if any(n.endswith(".bmd") for n in names):
        return "several loose models without the MKDD Patcher layout (files/MRAM.arc/mram/driver/<character>/): which is the driver is unknown"
    if names and all(n.endswith((".wav", ".aw", ".baa", ".bsft", ".bstn", ".ini", ".txt")) for n in names):
        return "sound files only: no character model"
    return "no files/MRAM.arc/mram/driver (MKDD Patcher layout) and no character.ini"


def scan(folder):
    """the characters in a mod folder (MKDD Patcher layout or this tool's)"""
    folder = unwrap(folder)
    ini = find_ci(folder, "character.ini")
    if ini:
        c = ini_read(ini)
        b = base_index(c.get("base", ""))
        if b is None:
            raise ValueError(f"{ini}: base= is missing or unknown")
        files = {n: os.path.join(folder, n) for n in os.listdir(folder) if n.lower() != "character.ini"}
        return [Character(b, c.get("name", BASES[b][1]), c.get("author", ""), c.get("source", ""), files, folder,
                          base_index(c["slot"]) if c.get("slot") else None)]
    files = find_ci(folder, "files") or folder
    drivers = find_ci(files, *DRIVERS)
    if not drivers:
        bmds = [os.path.join(d, f) for d, _, fs in os.walk(folder) for f in fs if f.lower().endswith(".bmd")]
        if len(bmds) == 1:
            # a loose model ("Tuxedo Mario/driver (7).bmd"): named after its folder, on Mario until
            # gather finds its skeleton's character
            name = re.sub(r"\s*driver[\s_(]*\d*[\s_)]*$", "", os.path.basename(folder), flags=re.I).strip() or "Character"
            c = Character(0, name, "", "", {"driver.bmd": bmds[0]}, f"{folder} (loose model)")
            c.loose = True
            return [c]
        raise ValueError(f"{folder}: {not_a_mod(folder)}")
    info = ini_read(find_ci(folder, "modinfo.ini"))
    author, modname = info.get("author", ""), info.get("modname", "")
    found = [(b, d) for d in sorted(os.listdir(drivers)) for b in [base_of_folder(d)] if b is not None and find_ci(drivers, d, "driver.bmd")]
    crow = len(found) == 20 and author.lower() == "crow8028"
    faces, names, anims = find_ci(files, *FACES), find_ci(files, *NAMES), find_ci(files, *ANIMS)
    voice = find_ci(folder, "audio_waves")
    kd = find_ci(files, *KARTS)
    karts = sorted(k for k in os.listdir(kd) if os.path.isdir(os.path.join(kd, k))) if kd else []
    audiores = find_ci(files, "AudioRes")
    if audiores and not find_ci(audiores, "GCKart.baa"):
        audiores = None
    out = []
    for b, d in found:
        dfolder = os.path.join(drivers, d)
        cf = {n: os.path.join(dfolder, n) for n in os.listdir(dfolder) if os.path.isfile(os.path.join(dfolder, n))}
        for kind, path in (("face.bti", faces and find_ci(faces, BASES[b][3] + ".bti")),
                           ("map.bti", faces and find_ci(faces, BASES[b][4] + "_mapicon32.bti")),
                           ("name.bti", names and find_ci(names, "chaname_" + BASES[b][5] + ".bti"))):
            if path:
                cf[kind] = path
        a = anims and find_ci(anims, d)
        if a:
            cf["anims"] = a
        if voice and len(found) == 1:
            cf["voice"] = voice
        elif audiores and len(found) == 1:
            cf["audiores"] = audiores
        if crow:
            name = CROW_NAMES[b]
        elif d.lower() in PACK_NAMES.get(modname.lower(), {}):
            name = PACK_NAMES[modname.lower()][d.lower()]
        elif len(found) == 1 and modname:
            name = re.sub(r"\s+in\s+MKDD.*$", "", modname, flags=re.I)
        else:
            name = f"{BASES[b][1]} 2"
        c = Character(b, name, author, "", cf, f"{folder} ({d})")
        c.karts = karts
        out.append(c)
    return out


def check(c, disc):
    """c.notes: what the game will do with it"""
    n = c.notes = []
    if "driver.bmd" not in c.files:
        n.append(("error", "no driver.bmd: the model is required"))
        return
    try:
        mine = bmd_info(read(c.files["driver.bmd"]))
    except OSError as e:
        n.append(("error", f"driver.bmd: {e}"))
        return
    if not mine:
        n.append(("error", "driver.bmd is not a J3D model"))
        return
    if disc:
        orig = disc.driver_file(c.base, "driver.bmd")
        theirs = bmd_info(yaz0(orig)) if orig else None
        if theirs:
            renamed = [a for a, b in zip(mine["joints"], theirs["joints"]) if a != b]
            # the game (runtime/charpack.c variant_anim): another character's skeleton, by joint
            # names past the root, moves as that character
            other = None if mine["joints"][1:] == theirs["joints"][1:] else next(
                (i for i in range(len(BASES)) if disc.joints(i)[1:] == mine["joints"][1:]), None)
            if other is not None:
                n.append(("ok", f"skeleton is {BASES[other][1]}'s ({len(mine['joints'])} joints): it moves with "
                                f"{BASES[other][1]}'s animations, its voice and stats stay {BASES[c.base][1]}'s"))
            elif len(mine["joints"]) == len(theirs["joints"]) and renamed and "anims" not in c.files:
                # animations follow joints by place, not name (a model made from Mario's: mario_all_root)
                n.append(("info", f"skeleton matches {BASES[c.base][1]}'s ({len(mine['joints'])} joints), "
                                  f"{len(renamed)} renamed ({', '.join(renamed[:3])}): animations follow joints by place"))
            elif mine["joints"][1:] != theirs["joints"][1:]:
                n.append(("error", f"skeleton differs from {BASES[c.base][1]}'s ({len(mine['joints'])} joints, {len(theirs['joints'])} expected) "
                                   "and is no other character's: " + ("the game can't play its own animations"
                                                                       if "anims" in c.files else "the base's animations would twist it")))
            else:
                n.append(("ok", f"skeleton matches {BASES[c.base][1]}'s ({len(mine['joints'])} joints)"))
            if mine["textures"] == theirs["textures"]:
                n.append(("ok", f"textures match ({len(mine['textures'])}): the select screen preview shows them"))
            elif len(mine["textures"]) == len(theirs["textures"]):
                n.append(("info", "texture names differ, same count: the preview matches them by place"))
            else:
                n.append(("info", f"{len(mine['textures'])} textures, {len(theirs['textures'])} expected: fine (the select screen "
                                  "builds its own model; only if that fails does it show its textures on the game's)"))
    else:
        n.append(("info", "no disc image given: skeleton and textures not checked"))
    for f, what in (("face.bti", "no face: the game's face is used"),
                    ("map.bti", "no map icon: the game makes one from the face"),
                    ("name.bti", "no name tag: the game writes the name")):
        if f not in c.files:
            n.append(("info" if f != "face.bti" else "warn", what))
    if "voice" in c.files or "audiores" in c.files:
        voice_check(c, disc, n)
    if getattr(c, "karts", None):
        n.append(("info", f"kart: the mod's kart{'s' if len(c.karts) > 1 else ''} ({', '.join(c.karts)}) not used yet: "
                          "it drives the game's"))


def folder_ids(chars):
    """each character's export folder name (c.id, made unique)"""
    used, out = set(), []
    for c in chars:
        cid = c.id
        while cid in used:
            cid += "-2"
        used.add(cid)
        out.append(cid)
    return out


def layout(chars):
    """[(cell, page)] for chars as the game places them once exported (runtime/charpack.c layout:
    folders in name order; pinned ones in their slot; the others in their own cell up to
    ceil(n / 20) each, the rest each in the cell with the fewest); page 1 is the game's own"""
    ids = folder_ids(chars)
    order = sorted(range(len(chars)), key=lambda i: ids[i].lower())
    count, share = [0] * len(BASES), -(-len(chars) // len(BASES))
    cell = [None] * len(chars)
    for i in order:
        if chars[i].slot is not None:
            cell[i] = chars[i].slot
            count[cell[i]] += 1
            share = max(share, count[cell[i]])
    for i in order:
        if cell[i] is None and count[chars[i].base] < share:
            cell[i] = chars[i].base
            count[cell[i]] += 1
    for i in order:
        if cell[i] is None:
            cell[i] = min(range(len(BASES)), key=lambda k: (count[k], k))
            count[cell[i]] += 1
    page, seen = [0] * len(chars), [0] * len(BASES)
    for i in order:
        seen[cell[i]] += 1
        page[i] = seen[cell[i]] + 1
    return list(zip(cell, page))


def model_hash(c):
    try:
        return hashlib.md5(read(c.files["driver.bmd"])).hexdigest()
    except (KeyError, OSError):
        return None


def rejected_reason(c):
    """why the game can't use c (after check), or None"""
    errors = [t for l, t in c.notes if l == "error"]
    return "; ".join(errors) if errors else None


def is_mod(folder):
    f = unwrap(folder)
    return bool(find_ci(f, "character.ini") or find_ci(f, "files"))


def place_loose(c, disc):
    """a loose model's base: the character whose skeleton it has (joint names past the root), else
    Mario, marked unknown"""
    c.unknown = True
    try:
        mine = bmd_info(read(c.files["driver.bmd"]))
    except OSError:
        mine = None
    if not disc or not mine:
        return
    for i in range(len(BASES)):
        if disc.joints(i)[1:] == mine["joints"][1:]:
            c.base, c.unknown = i, False
            return


def gather(paths, disc, have=()):
    """characters from mod folders or folders of mod folders: (accepted, [(what, reason)])"""
    hashes = {h for h in (model_hash(c) for c in have) if h}
    ok, bad = [], []
    todo = []
    for p in paths:
        if not os.path.isdir(p):
            bad.append((p, "not a folder (unzip it first)"))
        elif is_mod(p):
            todo.append(p)
        else:
            subs = [os.path.join(p, d) for d in sorted(os.listdir(p)) if os.path.isdir(os.path.join(p, d))]
            if not subs:
                todo.append(p)  # scan says what is missing
            todo += subs
    for p in todo:
        try:
            found = scan(p)
        except (OSError, ValueError) as e:
            bad.append((os.path.basename(p), str(e).replace(p + ": ", "")))
            continue
        for c in found:
            if c.loose:
                place_loose(c, disc)
            check(c, disc)
            if c.loose:
                c.notes.insert(0, ("warn", "a loose model: its character is unknown (no disc image, or no character has its skeleton): "
                                           "choose it under On") if c.unknown else
                               ("ok", f"a loose model: its skeleton is {BASES[c.base][1]}'s, so it plays as {BASES[c.base][1]}"))
            why = rejected_reason(c)
            h = model_hash(c)
            if not why and h in hashes:
                why = "already in the list (the same model)"
            if why:
                bad.append((f"{c.name} on {BASES[c.base][1]} ({os.path.basename(p)})", why))
                continue
            hashes.add(h)
            ok.append(c)
    return ok, bad


def export(chars, out):
    """character folders in out, and CREDITS.txt"""
    os.makedirs(out, exist_ok=True)
    for c, cid in zip(chars, folder_ids(chars)):
        dest = os.path.join(out, cid)
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        os.makedirs(dest)
        for name, src in c.files.items():
            if name in ("voice", "audiores"):  # check() made c.voice: the sounds the game plays
                for arc, wid, rate, v in getattr(c, "voice", []):
                    os.makedirs(os.path.join(dest, "voice", arc), exist_ok=True)
                    write_wav(os.path.join(dest, "voice", arc, f"{wid}.wav"), rate, v)
            elif os.path.isdir(src):
                shutil.copytree(src, os.path.join(dest, name))
            else:
                with open(os.path.join(dest, name), "wb") as f:
                    f.write(read(src))
        with open(os.path.join(dest, "character.ini"), "w", encoding="utf-8", newline="\n") as f:
            f.write(f"name={c.name}\nbase={BASE_IDS[c.base]}\nauthor={c.author}\nsource={c.source}\n")
            if c.slot is not None:
                f.write(f"slot={BASE_IDS[c.slot]}\n")
    with open(os.path.join(out, "CREDITS.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("MKDD-NX character pack\n\n")
        for c in chars:
            f.write(f"{c.name} (on {BASES[c.base][1]}): {c.author or 'unknown author'}{'  ' + c.source if c.source else ''}\n")


# ---- command line ----
def cli(args):
    iso = out = None
    folders = []
    it = iter(args)
    for a in it:
        if a == "--iso":
            iso = next(it)
        elif a == "--out":
            out = next(it)
        elif a != "--cli":
            folders.append(a)
    disc = Disc(iso) if iso else None
    chars, rejected = gather(folders, disc)
    for c, (cell, page) in zip(chars, layout(chars)):
        print(f"{c.name} (on {BASES[c.base][1]}, shown in {BASES[cell][1]}'s cell, page {page}) by {c.author or '?'}  [{c.src}]")
        for level, text in c.notes:
            print(f"   {level:5} {text}")
    for what, why in rejected:
        print(f"REJECTED {what}: {why}")
    bad = len(rejected)
    if out:
        export(chars, out)
        print(f"exported {len(chars)} characters to {out}")
    return 1 if bad else 0


# ---- the window ----
def enable_drop(root, on_drop):
    """folders dropped on the window (Windows: WM_DROPFILES; Tk has no drag and drop of its own).
    Elsewhere, nothing: the Add button still works."""
    if sys.platform != "win32":
        return
    import ctypes
    from ctypes import wintypes
    user32, shell32 = ctypes.windll.user32, ctypes.windll.shell32
    WM_DROPFILES, GWLP_WNDPROC = 0x0233, -4
    LRESULT = ctypes.c_ssize_t
    WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
    user32.CallWindowProcW.restype = LRESULT
    user32.CallWindowProcW.argtypes = [ctypes.c_void_p, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    setp = getattr(user32, "SetWindowLongPtrW", user32.SetWindowLongW)
    setp.restype, setp.argtypes = ctypes.c_void_p, [wintypes.HWND, ctypes.c_int, ctypes.c_void_p]
    shell32.DragQueryFileW.argtypes = [ctypes.c_void_p, wintypes.UINT, ctypes.c_wchar_p, wintypes.UINT]
    shell32.DragFinish.argtypes = [ctypes.c_void_p]
    shell32.DragAcceptFiles.argtypes = [wintypes.HWND, wintypes.BOOL]
    root.update_idletasks()
    hwnd = int(root.wm_frame(), 16)
    old = None

    def proc(h, msg, wp, lp):
        if msg == WM_DROPFILES:
            n = shell32.DragQueryFileW(wp, 0xFFFFFFFF, None, 0)
            paths = []
            for i in range(n):
                buf = ctypes.create_unicode_buffer(32768)
                shell32.DragQueryFileW(wp, i, buf, 32768)
                paths.append(buf.value)
            shell32.DragFinish(wp)
            root.after(10, lambda: on_drop(paths))
            return 0
        return user32.CallWindowProcW(old, h, msg, wp, lp)

    cb = WNDPROC(proc)
    root._drop_proc = cb  # kept alive with the window
    old = setp(hwnd, GWLP_WNDPROC, ctypes.cast(cb, ctypes.c_void_p))
    shell32.DragAcceptFiles(hwnd, True)


def gui(root=None):
    """the window; in root, a frame (MKDD-NX Tools' tab, tools/mkddnx_tools.py), when given: then
    returns the tool (its can_close())"""
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox

    cfg_path = os.path.join(os.path.expanduser("~"), ".mkdd_nx_charpack.json")
    try:
        cfg = json.load(open(cfg_path))
    except (OSError, ValueError):
        cfg = {}

    own = root is None
    root = root or tk.Tk()
    if own:
        root.title("MKDD-NX Characters")
        root.geometry("980x640")
    chars = []
    state = {"disc": None}

    top = ttk.Frame(root, padding=8)
    top.pack(fill="x")
    iso_v, out_v = tk.StringVar(value=cfg.get("iso", "")), tk.StringVar(value=cfg.get("out", ""))

    def save_cfg():
        cfg.update(iso=iso_v.get(), out=out_v.get())
        try:
            json.dump(cfg, open(cfg_path, "w"))
        except OSError:
            pass

    def pick_iso():
        p = filedialog.askopenfilename(title="Your MKDD disc image (only read)", filetypes=[("Disc image", "*.iso *.gcm"), ("All", "*.*")])
        if p:
            iso_v.set(p)
            open_disc()

    def open_disc():
        state["disc"] = None
        if iso_v.get():
            try:
                state["disc"] = Disc(iso_v.get())
            except (OSError, ValueError) as e:
                messagebox.showerror("Disc image", str(e))
        save_cfg()
        recheck()

    def pick_out():
        p = filedialog.askdirectory(title="Where to write the character folders")
        if p:
            out_v.set(p)
            save_cfg()

    for r, (label, var, cmd) in enumerate((("Disc image (to check against):", iso_v, pick_iso), ("Output folder:", out_v, pick_out))):
        ttk.Label(top, text=label).grid(row=r, column=0, sticky="w")
        ttk.Entry(top, textvariable=var, width=90).grid(row=r, column=1, sticky="we", padx=4)
        ttk.Button(top, text="Browse…", command=cmd).grid(row=r, column=2)
    top.columnconfigure(1, weight=1)

    bar = ttk.Frame(root, padding=(8, 0))
    bar.pack(fill="x")
    mid = ttk.PanedWindow(root, orient="vertical")
    mid.pack(fill="both", expand=True, padx=8, pady=8)
    tree = ttk.Treeview(mid, columns=("name", "base", "shown", "author", "status", "from"), show="headings", selectmode="browse")
    for col, title, w in (("name", "Name", 170), ("base", "Plays as", 110), ("shown", "Shown in (cell, page)", 170),
                          ("author", "Author", 120), ("status", "Status", 70), ("from", "From", 320)):
        tree.heading(col, text=title)
        tree.column(col, width=w)
    mid.add(tree, weight=3)

    detail = ttk.Frame(mid, padding=4)
    mid.add(detail, weight=2)
    name_v, base_v, author_v, source_v, slot_v = tk.StringVar(), tk.StringVar(), tk.StringVar(), tk.StringVar(), tk.StringVar()
    fields = ttk.Frame(detail)
    fields.pack(fill="x")
    for col, (label, var) in enumerate((("Name", name_v), ("Author", author_v), ("Source link", source_v))):
        ttk.Label(fields, text=label).grid(row=0, column=2 * col, sticky="w")
        ttk.Entry(fields, textvariable=var, width=28).grid(row=0, column=2 * col + 1, padx=4)
    ttk.Label(fields, text="On").grid(row=0, column=6)
    base_box = ttk.Combobox(fields, textvariable=base_v, values=[b[1] for b in BASES], state="readonly", width=16)
    base_box.grid(row=0, column=7, padx=4)
    ttk.Label(fields, text="Cell").grid(row=0, column=8)
    AUTO = "Automatic"
    slot_box = ttk.Combobox(fields, textvariable=slot_v, values=[AUTO] + [b[1] for b in BASES], state="readonly", width=16)
    slot_box.grid(row=0, column=9, padx=4)
    notes = tk.Text(detail, height=8, wrap="word")
    notes.pack(fill="both", expand=True, pady=(6, 0))
    for level, colour in (("ok", "#1a7f37"), ("info", "#555555"), ("warn", "#9a6700"), ("error", "#cf222e")):
        notes.tag_configure(level, foreground=colour)

    def status(c):
        levels = [l for l, _ in c.notes]
        return "error" if "error" in levels else "check" if "warn" in levels else "ok"

    def refresh():
        tree.delete(*tree.get_children())
        places = layout(chars)
        for i, (c, (cell, page)) in enumerate(zip(chars, places)):
            shown = f"{BASES[cell][1]}, page {page}" + (" (pinned)" if c.slot is not None else "")
            tree.insert("", "end", iid=str(i), values=(c.name, BASES[c.base][1], shown, c.author, status(c), c.src))
        pages = max((p for _, p in places), default=1)
        count_v.set(f"{len(chars)} characters, {pages} pages on the select screen (page 1: the game's own)")

    def recheck():
        for c in chars:
            check(c, state["disc"])
        refresh()

    def current():
        s = tree.selection()
        return chars[int(s[0])] if s else None

    def show(_=None):
        c = current()
        notes.delete("1.0", "end")
        if not c:
            return
        name_v.set(c.name), author_v.set(c.author), source_v.set(c.source), base_v.set(BASES[c.base][1])
        slot_v.set(AUTO if c.slot is None else BASES[c.slot][1])
        for level, text in c.notes:
            notes.insert("end", f"{level.upper():6}{text}\n", level)
        notes.insert("end", "\nFiles: " + ", ".join(sorted(c.files)))

    def apply(*_):
        c = current()
        if not c:
            return
        c.name, c.author, c.source = name_v.get().strip() or c.name, author_v.get().strip(), source_v.get().strip()
        b = base_index(base_v.get())
        if b is not None and b != c.base:
            c.base, c.unknown = b, False
            check(c, state["disc"])
        c.slot = None if slot_v.get() in ("", AUTO) else base_index(slot_v.get())
        sel = tree.selection()
        refresh()
        tree.selection_set(sel)

    def report(added, rejected):
        """what was added and what was rejected, in a window of its own when there is a lot"""
        text = f"Added {added} character{'s' if added != 1 else ''}."
        if rejected:
            text += f"\n\nRejected {len(rejected)}:\n" + "\n".join(f"- {w}: {r}" for w, r in rejected)
        if len(rejected) <= 6:
            (messagebox.showwarning if rejected else messagebox.showinfo)("Add", text)
            return
        win = tk.Toplevel(root)
        win.title("Added and rejected")
        win.geometry("760x420")
        t = tk.Text(win, wrap="word")
        t.insert("1.0", text)
        t.configure(state="disabled")
        t.pack(fill="both", expand=True, padx=8, pady=8)
        ttk.Button(win, text="OK", command=win.destroy).pack(pady=(0, 8))

    def add_paths(paths):
        root.config(cursor="watch")
        root.update_idletasks()
        try:
            found, rejected = gather(paths, state["disc"], chars)
        finally:
            root.config(cursor="")
        chars.extend(found)
        refresh()
        report(len(found), rejected)

    def add():
        p = filedialog.askdirectory(title="A character mod, or a folder of character mods")
        if p:
            add_paths([p])

    def remove():
        c = current()
        if c:
            chars.remove(c)
            refresh()

    def do_export():
        if not out_v.get():
            pick_out()
        if not out_v.get() or not chars:
            return
        bad = [c.name for c in chars if status(c) == "error"]
        if bad and not messagebox.askyesno("Export", "These have errors and may not work:\n" + "\n".join(bad) + "\n\nExport anyway?"):
            return
        try:
            export(chars, out_v.get())
        except OSError as e:
            messagebox.showerror("Export", str(e))
            return
        messagebox.showinfo("Export", f"{len(chars)} characters written to\n{out_v.get()}\n\n"
                            "Copy those folders to the SD card's /switch/MKDD-NX/mods/characters/.")

    for text, cmd in (("Add mod or folder of mods…", add), ("Remove", remove), ("Check again", recheck), ("Export…", do_export)):
        ttk.Button(bar, text=text, command=cmd).pack(side="left", padx=(0, 6))
    count_v = tk.StringVar(value="Drop mod folders (or a folder of them) on this window" if sys.platform == "win32"
                           else "Add mod folders (or a folder of them)")
    ttk.Label(bar, textvariable=count_v).pack(side="left", padx=12)
    enable_drop(root.winfo_toplevel(), lambda paths: root.winfo_ismapped() and add_paths(paths))  # (in a tab: while shown)
    tree.bind("<<TreeviewSelect>>", show)
    for v in (name_v, author_v, source_v):
        v.trace_add("write", lambda *_: None)
    for w in fields.winfo_children():
        if isinstance(w, ttk.Entry):
            w.bind("<Return>", apply)
            w.bind("<FocusOut>", apply)
    base_box.bind("<<ComboboxSelected>>", apply)
    slot_box.bind("<<ComboboxSelected>>", apply)
    if iso_v.get():
        root.after(100, open_disc)
    if own:
        root.mainloop()
    return types.SimpleNamespace(can_close=lambda: True)


if __name__ == "__main__":
    if "--cli" in sys.argv:
        sys.exit(cli(sys.argv[1:]))
    gui()
