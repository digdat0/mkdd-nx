"""Builds the Switch app icon and loading-screen logo from a square PNG.
usage: py -3 tools/make_logo.py <logo.png>
writes switch/icon.jpg (256x256, what nx_create_nro wants) and switch/logo_512.rgba (raw RGBA
rows, top row first, embedded by switch/logo.S)."""
import sys
from pathlib import Path
from PIL import Image

here = Path(__file__).resolve().parent.parent / "switch"
im = Image.open(sys.argv[1]).convert("RGBA")
flat = Image.new("RGBA", im.size, (255, 255, 255, 255))
flat.alpha_composite(im)
flat.convert("RGB").resize((256, 256), Image.LANCZOS).save(here / "icon.jpg", quality=92)
(here / "logo_512.rgba").write_bytes(flat.resize((512, 512), Image.LANCZOS).tobytes())
print("wrote", here / "icon.jpg", "and", here / "logo_512.rgba")
