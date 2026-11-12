"""Drive the offline fixture in an isolated 1200x800 Xvfb display."""
import ctypes
import os
import subprocess
import sys
import tempfile
import time

x11 = ctypes.CDLL("libX11.so.6")
xtst = ctypes.CDLL("libXtst.so.6")
D = ctypes.c_void_p
U = ctypes.c_ulong
P = ctypes.POINTER


class WindowAttributes(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int),
        ("height", ctypes.c_int), ("border_width", ctypes.c_int), ("depth", ctypes.c_int),
        ("visual", D), ("root", U), ("window_class", ctypes.c_int),
        ("bit_gravity", ctypes.c_int), ("win_gravity", ctypes.c_int),
        ("backing_store", ctypes.c_int), ("backing_planes", U), ("backing_pixel", U),
        ("save_under", ctypes.c_int), ("colormap", U), ("map_installed", ctypes.c_int),
        ("map_state", ctypes.c_int), ("all_event_masks", ctypes.c_long),
        ("your_event_mask", ctypes.c_long), ("do_not_propagate_mask", ctypes.c_long),
        ("override_redirect", ctypes.c_int), ("screen", D),
    ]


x11.XGetWindowAttributes.argtypes = [D, U, P(WindowAttributes)]
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
x11.XOpenDisplay.restype = D
x11.XDefaultRootWindow.argtypes = [D]
x11.XDefaultRootWindow.restype = U
x11.XQueryTree.argtypes = [D, U, P(U), P(U), P(P(U)), P(ctypes.c_uint)]
x11.XFetchName.argtypes = [D, U, P(ctypes.c_char_p)]
x11.XFree.argtypes = [D]
x11.XSetInputFocus.argtypes = [D, U, ctypes.c_int, U]
x11.XFlush.argtypes = [D]
x11.XCloseDisplay.argtypes = [D]
x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
x11.XStringToKeysym.restype = U
x11.XKeysymToKeycode.argtypes = [D, U]
x11.XKeysymToKeycode.restype = ctypes.c_uint
xtst.XTestFakeMotionEvent.argtypes = [D, ctypes.c_int, ctypes.c_int, ctypes.c_int, U]
xtst.XTestFakeButtonEvent.argtypes = [D, ctypes.c_uint, ctypes.c_int, U]
xtst.XTestFakeKeyEvent.argtypes = [D, ctypes.c_uint, ctypes.c_int, U]


def find_window(display):
    root = x11.XDefaultRootWindow(display)
    output_root, parent = U(), U()
    children, count = P(U)(), ctypes.c_uint()
    if not x11.XQueryTree(display, root, ctypes.byref(output_root), ctypes.byref(parent), ctypes.byref(children), ctypes.byref(count)):
        raise RuntimeError("cannot inspect X11 window tree")
    result = None
    try:
        for index in range(count.value):
            name = ctypes.c_char_p()
            if x11.XFetchName(display, children[index], ctypes.byref(name)):
                if name.value == b"rustGram offline fixture":
                    result = children[index]
                x11.XFree(name)
    finally:
        x11.XFree(children)
    return result


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: drive_gui_fixture.py PATH_TO_GUI_FIXTURE")
    env = os.environ.copy()
    for name in ("WAYLAND_DISPLAY", "RUSTGRAM_API_ID", "RUSTGRAM_API_HASH"):
        env.pop(name, None)
    env["WINIT_UNIX_BACKEND"] = "x11"
    display = x11.XOpenDisplay(None)
    if not display:
        raise RuntimeError("X11 display unavailable")
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen([sys.argv[1]], stdout=log, stderr=log, env=env)
        try:
            deadline = time.monotonic() + 15
            window = None
            while time.monotonic() < deadline and process.poll() is None:
                window = find_window(display)
                if window:
                    attributes = WindowAttributes()
                    if x11.XGetWindowAttributes(display, window, ctypes.byref(attributes)) and attributes.map_state == 2:
                        break
                time.sleep(0.1)
            else:
                raise RuntimeError("fixture window did not become viewable")
            x11.XSetInputFocus(display, window, 1, 0)
            x11.XFlush(display)
            time.sleep(0.5)

            def click(x, y):
                xtst.XTestFakeMotionEvent(display, -1, x, y, 0)
                xtst.XTestFakeButtonEvent(display, 1, 1, 0)
                xtst.XTestFakeButtonEvent(display, 1, 0, 0)
                x11.XFlush(display)
                time.sleep(0.2)

            click(350, 595)
            for char in "hello":
                key = x11.XKeysymToKeycode(display, x11.XStringToKeysym(char.encode()))
                xtst.XTestFakeKeyEvent(display, key, 1, 0)
                xtst.XTestFakeKeyEvent(display, key, 0, 0)
            x11.XFlush(display)
            time.sleep(0.5)
            click(310, 629)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                log.seek(0)
                output = log.read()
                if b"OFFLINE_SEND_RECEIVED" in output and b"OFFLINE_HISTORY_REFRESHED" in output:
                    print("PASS: offline GUI accepted text and refreshed history")
                    return
                time.sleep(0.1)
            raise RuntimeError("fixture did not observe send and history refresh")
        finally:
            process.terminate()
            process.wait(timeout=5)
            x11.XCloseDisplay(display)


if __name__ == "__main__":
    main()
