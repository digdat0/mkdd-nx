"""MKDD-NX Tools' updater (the Start page's Updates box): asks GitHub for the latest release
(api.github.com/repos/<repo>/releases/latest, no update.txt) and its MKDD-NX-Tools.exe asset's size and SHA-256
("digest", recorded by GitHub on upload). One click checks, and when the release is newer (or the same version with a
different .exe: a rebuild) downloads it with a progress bar, checks it, puts it in place of the running .exe (renamed
to MKDD-NX-Tools.exe.old, deleted at the next start) and starts it. Only the packaged Windows .exe replaces itself;
from source or on macOS the button opens the release page. No token: the repository has to be public while updating.
The automatic check at start is off by default (settings.json in %APPDATA%\\MKDD-NX-Tools)."""
import hashlib
import json
import os
import subprocess
import sys
import threading
import tkinter as tk
import urllib.error
import urllib.request
import webbrowser
from pathlib import Path
from tkinter import ttk

import ui_theme

REPO = "digdat0/mkdd-nx"
API = f"https://api.github.com/repos/{REPO}/releases/latest"
ASSET_URL = f"https://github.com/{REPO}/releases/download/"
RELEASES = f"https://github.com/{REPO}/releases/latest"
ASSET = "MKDD-NX-Tools.exe"
FROZEN_WIN = getattr(sys, "frozen", False) and sys.platform == "win32"


def version():
    """the version packaged with the .exe (tools_version.txt, written by build_tools_app.py), else the source tree's"""
    try:
        if getattr(sys, "frozen", False):
            return (Path(sys._MEIPASS) / "tools_version.txt").read_text().strip()
        for line in (Path(__file__).resolve().parent.parent / "switch" / "CMakeLists.txt").read_text().splitlines():
            if line.startswith('set(MKDD_VERSION "'):
                return line.split('"')[1]
    except (OSError, IndexError, AttributeError):
        pass
    return "0.0.0"


def version_cmp(a, b):
    def parts(v):
        out = []
        for p in (v.split(".") + ["0"] * 4)[:4]:
            n = "".join(ch for ch in p if ch.isdigit())
            out.append(int(n) if n else 0)
        return out
    pa, pb = parts(a), parts(b)
    return (pa > pb) - (pa < pb)


def _settings_path():
    base = os.environ.get("APPDATA") if sys.platform == "win32" else None
    base = Path(base) if base else Path.home() / (".config" if sys.platform != "darwin" else "Library/Application Support")
    return base / "MKDD-NX-Tools" / "settings.json"


def load_settings():
    try:
        return json.loads(_settings_path().read_text())
    except (OSError, ValueError):
        return {}


def save_settings(s):
    p = _settings_path()
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(s, indent=1))
    except OSError:
        pass


def cleanup_old():
    """the .exe an update replaced (still running for a moment after the update started the new one: next time)"""
    if FROZEN_WIN:
        try:
            Path(sys.executable + ".old").unlink(missing_ok=True)
        except OSError:
            pass


def _get(url, timeout=20):
    req = urllib.request.Request(url, headers={"User-Agent": f"MKDD-NX-Tools/{version()}"})
    if url.startswith("https://api.github.com/"):
        req.add_header("Accept", "application/vnd.github+json")
        req.add_header("X-GitHub-Api-Version", "2022-11-28")
    return urllib.request.urlopen(req, timeout=timeout)


def _why(e):
    if isinstance(e, urllib.error.HTTPError):
        if e.code == 404:
            return "No release found (is the repository public?)"
        if e.code in (403, 429):
            return "GitHub is busy: try again in an hour"
        return f"The server refused (HTTP {e.code}): try again later"
    if isinstance(e, (urllib.error.URLError, OSError)):
        return "No internet connection, or the connection timed out"
    return f"Update failed: {e}"


class Updater:
    """the check and the install on a worker thread; the panel polls .state"""

    def __init__(self):
        self.state, self.msg, self.latest, self.progress = "idle", "", "", 0.0
        self.rel = None
        self.busy = False

    def start(self, install=False):
        if self.busy:
            return
        self.busy = True
        self.state, self.msg, self.progress = ("downloading" if install else "checking"), "", 0.0
        threading.Thread(target=self._work, args=(install,), daemon=True).start()

    def _work(self, install):
        try:
            self._install() if install else self._check()
        except Exception as e:  # noqa: BLE001 (any failure ends as a message, never a crash)
            self.state, self.msg = "error", _why(e)
        finally:
            self.busy = False

    def _check(self):
        with _get(API) as r:
            rel = json.loads(r.read(512 << 10))
        tag = str(rel.get("tag_name", ""))
        self.latest = tag[1:] if tag[:1] in "vV" else tag
        a = next((a for a in rel.get("assets", []) if a.get("name") == ASSET), None)
        digest = (a or {}).get("digest") or ""
        url = (a or {}).get("browser_download_url", "")
        if not a or not digest.startswith("sha256:") or len(digest) != 71 or not url.startswith(ASSET_URL):
            self.state, self.msg = "error", "The latest release has no MKDD-NX-Tools.exe"
            return
        self.rel = dict(url=url, size=int(a.get("size", 0)), sha=digest[7:].lower())
        vc = version_cmp(self.latest, version())
        same = vc == 0 and (not FROZEN_WIN or _sha(sys.executable) == self.rel["sha"])
        self.state = "current" if same or vc < 0 else "available"

    def _install(self):
        exe = Path(sys.executable)
        dl, old = exe.with_name(exe.name + ".download"), exe.with_name(exe.name + ".old")
        h, got, size = hashlib.sha256(), 0, self.rel["size"]
        try:
            with _get(self.rel["url"], timeout=30) as r, open(dl, "wb") as f:
                while True:
                    chunk = r.read(256 << 10)
                    if not chunk:
                        break
                    f.write(chunk)
                    h.update(chunk)
                    got += len(chunk)
                    self.progress = got / size if size else 0.0
                    if size and got > size:
                        break
            if got != size or h.hexdigest() != self.rel["sha"] or _sha(dl) != self.rel["sha"]:
                raise ValueError("the download is damaged: try again")
            with open(dl, "rb") as f:
                if f.read(2) != b"MZ":
                    raise ValueError("the download is not MKDD-NX Tools")
            moved = False
            try:  # Windows lets a running .exe be renamed, not overwritten
                old.unlink(missing_ok=True)
                os.replace(exe, old)
                moved = True
                os.replace(dl, exe)
                self.swap_later = None
            except OSError:  # (held open): swapped by a script once this copy has exited
                if moved:  # the .exe back in its place, so closing without the restart leaves a working copy
                    os.replace(old, exe)
                self.swap_later = dl
        except Exception:
            dl.unlink(missing_ok=True)
            raise
        self.progress, self.state = 1.0, "ready"


def _sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def restart(swap_later=None):
    """start the (new) .exe as a fresh copy and end this one"""
    exe = sys.executable
    env = {k: v for k, v in os.environ.items() if not k.startswith(("_PYI", "_MEI"))}
    env["PYINSTALLER_RESET_ENVIRONMENT"] = "1"  # a new onefile instance, not this one's child
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    if swap_later:
        bat = Path(exe).with_name("MKDD-NX-Tools-update.cmd")
        # the paths go in by environment, not into the script's text: a % or a non-ASCII letter in them stays as it is
        env["MKDDNX_EXE"], env["MKDDNX_NEW"] = exe, str(swap_later)
        bat.write_text(
            "@echo off\r\n:wait\r\n"
            f'tasklist /FI "PID eq {os.getpid()}" | find "{os.getpid()}" >nul && (timeout /t 1 /nobreak >nul & goto wait)\r\n'
            'move /y "%MKDDNX_EXE%" "%MKDDNX_EXE%.old" >nul\r\nmove /y "%MKDDNX_NEW%" "%MKDDNX_EXE%" >nul\r\n'
            'start "" "%MKDDNX_EXE%"\r\n(goto) 2>nul & del "%~f0"\r\n')
        subprocess.Popen(["cmd", "/c", str(bat)], env=env, creationflags=flags, close_fds=True)
    else:
        subprocess.Popen([exe], env=env, close_fds=True)
    os._exit(0)


class UpdatePanel:
    """the Updates page: version, one button, a progress bar, the automatic check. notify(True) when a newer release is
    found or installed (the side bar's entry shows it)"""

    def __init__(self, parent, root, k, can_close, notify=lambda new: None):
        self.root, self.can_close, self.notify, self.up, self.go = root, can_close, notify, Updater(), False
        self.settings = load_settings()
        bg = ui_theme.FIELD
        box = tk.Frame(parent, background=bg, highlightthickness=1, highlightbackground=ui_theme.LINE)
        ttk.Label(parent, text="Updates", font=(ui_theme.FONT, 18, "bold")).pack(anchor="w")
        ttk.Label(parent, text="One click checks GitHub for the latest MKDD-NX Tools and, when there is a newer one, downloads it, "
                  "replaces this .exe and starts it again. The previous .exe is kept as MKDD-NX-Tools.exe.old until the next "
                  "start.", foreground=ui_theme.DIM, wraplength=int(760 * k), justify="left").pack(anchor="w", pady=(2, int(12 * k)))
        box.pack(fill="x", anchor="n")
        box.columnconfigure(1, weight=1)
        tk.Label(box, text="MKDD-NX Tools", background=bg, foreground=ui_theme.TEXT_C, font=(ui_theme.FONT, 11, "bold")).grid(
            row=0, column=0, sticky="w", padx=int(14 * k), pady=(int(10 * k), 0))
        self.info = tk.Label(box, text=f"This version: {version()}", background=bg, foreground=ui_theme.DIM_C,
                             font=(ui_theme.FONT, 9), anchor="w")
        self.info.grid(row=0, column=1, sticky="w", pady=(int(10 * k), 0))
        self.btn = ttk.Button(box, text="Check for updates", style="Accent.TButton", command=self.click)
        self.btn.grid(row=0, column=2, rowspan=2, sticky="e", padx=int(14 * k), pady=int(10 * k))
        self.bar = ttk.Progressbar(box, maximum=1000)
        self.status = tk.Label(box, text="" if FROZEN_WIN else "Running from source or on macOS: the button opens the release page.",
                               background=bg, foreground=ui_theme.DIM_C, font=(ui_theme.FONT, 9), anchor="w")
        self.status.grid(row=1, column=0, columnspan=2, sticky="ew", padx=int(14 * k))
        self.auto = tk.BooleanVar(value=bool(self.settings.get("auto_update", False)))
        tk.Checkbutton(box, text="Check for updates when MKDD-NX Tools starts", variable=self.auto, command=self.save_auto,
                       background=bg, foreground=ui_theme.TEXT_C, activebackground=bg, activeforeground=ui_theme.TEXT_C,
                       selectcolor=ui_theme.BG, font=(ui_theme.FONT, 9), anchor="w").grid(
            row=3, column=0, columnspan=3, sticky="w", padx=int(10 * k), pady=(int(2 * k), int(8 * k)))
        self.k, self.last = k, None
        cleanup_old()
        if self.auto.get():
            self.up.start()
        self.poll()

    def save_auto(self):
        self.settings["auto_update"] = bool(self.auto.get())
        save_settings(self.settings)

    def click(self):
        if not FROZEN_WIN:
            webbrowser.open(RELEASES)
            return
        if self.up.state == "ready":
            self.finish()
            return
        self.go = True
        self.up.start(install=self.up.state == "available")

    def finish(self):
        if not self.can_close():  # (a tool with unsaved work said no)
            self.status.configure(text="The update is installed: it starts when you close and open MKDD-NX Tools again.")
            return
        restart(getattr(self.up, "swap_later", None))

    def poll(self):
        u, k = self.up, self.k
        cur = (u.state, u.msg, u.latest, round(u.progress * 1000))
        if cur != self.last:
            self.last = cur
            if u.latest:
                self.info.configure(text=f"This version: {version()}    Latest release: {u.latest}")
            show_bar = u.state in ("downloading", "ready")
            if show_bar:
                self.bar.grid(row=2, column=0, columnspan=2, sticky="ew", padx=int(14 * k), pady=(int(4 * k), 0))
                self.bar["value"] = int(u.progress * 1000)
            else:
                self.bar.grid_remove()
            self.notify(u.state in ("available", "ready"))
            busy = u.state in ("checking", "downloading")
            self.btn.state(["disabled"] if busy else ["!disabled"])
            if u.state == "checking":
                self.btn.configure(text="Checking..."), self.status.configure(text="Connecting to GitHub...")
            elif u.state == "current":
                self.go = False
                self.btn.configure(text="Check for updates"), self.status.configure(text="You have the latest version.")
            elif u.state == "available":
                self.btn.configure(text=f"Update to {u.latest}")
                self.status.configure(text=f"Version {u.latest} is available.")
                if self.go:
                    u.start(install=True)
            elif u.state == "downloading":
                self.btn.configure(text="Downloading...")
                self.status.configure(text=f"Downloading {u.latest}... {int(u.progress * 100 + 0.5)}%")
            elif u.state == "ready":
                self.btn.configure(text="Restart now")
                self.status.configure(text=f"Version {u.latest} is installed. Restarting...")
                if self.go:
                    self.root.after(600, self.finish)
                else:
                    self.status.configure(text="The update is installed: restart to use it.")
            elif u.state == "error":
                self.go = False
                self.btn.configure(text="Check for updates"), self.status.configure(text=u.msg)
        self.root.after(100, self.poll)
