"""PyInstaller, run by tools/pyi_app.py. Python 3.10.0's dis.get_instructions reads past the constants
of some code (fixed in 3.10.1; Pillow 12's PIL.Image hits it), and PyInstaller scans every module
with it: there such a constant is taken as unknown instead of stopping the build."""
import sys

if sys.version_info[:3] == (3, 10, 0):
    import dis
    _const_info = dis._get_const_info

    def _get_const_info(const_index, const_list):
        try:
            return _const_info(const_index, const_list)
        except IndexError:
            return None, ""
    dis._get_const_info = _get_const_info

from PyInstaller.__main__ import run  # noqa: E402

if __name__ == "__main__":
    run(sys.argv[1:])
