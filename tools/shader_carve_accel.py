"""Optional zero-copy native scanner for shader_carve's read-only mapping."""

from contextlib import contextmanager
import os
from pathlib import Path
import sys


def _load_native(ctypes):
    name = {"win32": "shader_carve_native.dll", "linux": "libshader_carve_native.so",
            "darwin": "libshader_carve_native.dylib"}.get(sys.platform)
    if name is None:
        return None
    path = Path(__file__).resolve().parent / "shader_carve_native" / "target" / "release" / name
    if not path.is_file():
        return None
    scan = ctypes.CDLL(str(path)).leonida_shader_carve_find_v1
    scan.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t]
    scan.restype = ctypes.c_size_t
    return scan


def _acquire_buffer(ctypes, mapping):
    """Pin the mapping via CPython's stable Py_buffer ABI (3.11+)."""
    class Buffer(ctypes.Structure):
        _fields_ = [("buf", ctypes.c_void_p), ("obj", ctypes.c_void_p),
                    ("len", ctypes.c_ssize_t), ("itemsize", ctypes.c_ssize_t),
                    ("readonly", ctypes.c_int), ("ndim", ctypes.c_int),
                    ("format", ctypes.c_void_p), ("shape", ctypes.c_void_p),
                    ("strides", ctypes.c_void_p), ("suboffsets", ctypes.c_void_p),
                    ("internal", ctypes.c_void_p)]

    acquire = ctypes.pythonapi.PyObject_GetBuffer
    acquire.argtypes = [ctypes.py_object, ctypes.c_void_p, ctypes.c_int]
    acquire.restype = ctypes.c_int
    release = ctypes.pythonapi.PyBuffer_Release
    release.argtypes = [ctypes.c_void_p]
    release.restype = None
    view = Buffer()
    if acquire(mapping, ctypes.byref(view), 0) != 0:
        raise BufferError("cannot acquire shader archive buffer")
    return view, release


@contextmanager
def entry_search(mapping, pattern):
    """Yield the original regex match API, using native offsets when available."""
    def reference(pos):
        return pattern.search(mapping, pos)

    if (os.environ.get("LEONIDA_SHADER_CARVE_NATIVE", "").strip().lower() in ("0", "false", "off", "no")
            or sys.implementation.name != "cpython" or sys.version_info < (3, 11)):
        yield reference
        return

    view = release = None
    try:
        import ctypes

        scan = _load_native(ctypes)
        if scan is not None:
            view, release = _acquire_buffer(ctypes, mapping)
            if not view.readonly or view.len <= 0 or not view.buf:
                release(ctypes.byref(view))
                release = None
                scan = None
    except (ImportError, OSError, AttributeError, BufferError, TypeError, ValueError):
        scan = None

    if scan is None:
        yield reference
        return

    released = False

    def search(pos):
        nonlocal scan
        if (
            scan is not None
            and view.readonly
            and isinstance(pos, int)
            and not isinstance(pos, bool)
            and 0 <= pos < view.len
        ):
            if (match := pattern.match(mapping, pos)) is not None:
                return match
            try:
                offset = scan(view.buf, view.len, pos)
                if offset == view.len:
                    return None
                if pos <= offset < view.len and (match := pattern.match(mapping, offset)) is not None:
                    return match
            except (OSError, ValueError, TypeError, OverflowError, ctypes.ArgumentError):
                pass
            scan = None
        return reference(pos)

    try:
        yield search
    finally:
        scan = None
        if not released and release is not None and view is not None:
            released = True
            release(ctypes.byref(view))
