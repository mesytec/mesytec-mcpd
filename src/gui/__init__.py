import sys

_GUI_DEPS = {"PySide6", "pyqtgraph", "platformdirs"}


def _report_error(msg: str):
    if sys.platform == "win32" and (sys.stderr is None or not sys.stderr.isatty()):
        import ctypes

        MB_OK, MB_ICONERROR = 0x0, 0x10
        ctypes.windll.user32.MessageBoxW(None, msg, "mesytec-mcpd-gui", MB_OK | MB_ICONERROR)
    else:
        print(msg, file=sys.stderr)


def main():
    try:
        from .main_window import main as _main
    except ModuleNotFoundError as e:
        if (e.name or "").split(".")[0] not in _GUI_DEPS:
            raise
        _report_error(
            f"Missing module '{e.name}'.\n\n"
            "The GUI requires additional dependencies:\n"
            "    pip install mesytec-mcpd[gui]"
        )
        sys.exit(1)
    _main()


__all__ = ["main"]
