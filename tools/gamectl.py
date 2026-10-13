"""Drive a running game for testing: screenshot, key taps for menus, camera turns.

Usage (python -I gamectl.py <exe name> <cmd> ...):
  shot <out.png> [scale]       screenshot of the game window (default scale 0.5)
  key <name> [hold_ms]         tap a key by scancode (menus only: enter esc space up down left right e f q)
  turn <dx_per_step> <steps> [step_ms]   relative mouse moves = camera turn
  sweep <seconds> [speed]      turn the camera left/right in a smooth back-and-forth for N seconds
  click <x> <y>                left click at client coords (full-res pixels; menus only)
  focus                        bring the game window to the front
Camera-only rule: once loaded into the character, use only turn/sweep (mouse movement).
"""
import ctypes, struct, sys, time, zlib, math
from ctypes import wintypes as wt

U32 = ctypes.WinDLL('user32', use_last_error=True)
G32 = ctypes.WinDLL('gdi32')
K32 = ctypes.WinDLL('kernel32')
U32.SetProcessDPIAware()

sys.path.insert(0, __file__.rsplit('\\', 1)[0])
import n3mon

SC = {'enter': 0x1C, 'esc': 0x01, 'space': 0x39, 'up': 0xC8, 'down': 0xD0, 'left': 0xCB, 'right': 0xCD,
      'e': 0x12, 'f': 0x21, 'q': 0x10, 'tab': 0x0F, 'backspace': 0x0E}
EXT = {'up', 'down', 'left', 'right'}

class MOUSEINPUT(ctypes.Structure):
    _fields_ = [('dx', wt.LONG), ('dy', wt.LONG), ('mouseData', wt.DWORD), ('dwFlags', wt.DWORD),
                ('time', wt.DWORD), ('dwExtraInfo', ctypes.c_size_t)]
class KEYBDINPUT(ctypes.Structure):
    _fields_ = [('wVk', wt.WORD), ('wScan', wt.WORD), ('dwFlags', wt.DWORD), ('time', wt.DWORD),
                ('dwExtraInfo', ctypes.c_size_t)]
class _U(ctypes.Union):
    _fields_ = [('mi', MOUSEINPUT), ('ki', KEYBDINPUT), ('pad', ctypes.c_byte * 32)]
class INPUT(ctypes.Structure):
    _fields_ = [('type', wt.DWORD), ('u', _U)]

def send(*inputs):
    arr = (INPUT * len(inputs))(*inputs)
    U32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT))

def key(name, hold_ms=60):
    sc = SC[name]; fl = 0x0008 | (0x0001 if name in EXT else 0)
    down = INPUT(1); down.u.ki = KEYBDINPUT(0, sc & 0xFF, fl, 0, 0)
    up = INPUT(1); up.u.ki = KEYBDINPUT(0, sc & 0xFF, fl | 0x0002, 0, 0)
    send(down); time.sleep(hold_ms / 1000); send(up)

def click(h, x, y):
    pt = wt.POINT(int(x), int(y)); U32.ClientToScreen(h, ctypes.byref(pt)); U32.SetCursorPos(pt.x, pt.y); time.sleep(0.15)
    for fl in (0x2, 0x4):
        m = INPUT(0); m.u.mi = MOUSEINPUT(0, 0, 0, fl, 0, 0); send(m); time.sleep(0.08)

def move(dx, dy=0):
    m = INPUT(0); m.u.mi = MOUSEINPUT(int(dx), int(dy), 0, 0x0001, 0, 0); send(m)

EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
def find_window(pid):
    best = []
    def cb(h, _):
        p = wt.DWORD(); U32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and U32.IsWindowVisible(h):
            r = wt.RECT(); U32.GetClientRect(h, ctypes.byref(r))
            best.append((r.right * r.bottom, h))
        return True
    U32.EnumWindows(EnumWindowsProc(cb), 0)
    return max(best)[1] if best else None

def focus(h):
    U32.ShowWindow(h, 9); U32.SetForegroundWindow(h); time.sleep(0.3)

def png(path, w, h, rgb):
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

def shot(h, path, scale=0.5):
    """Capture the screen area of the window's client rect (works for borderless/windowed)."""
    r = wt.RECT(); U32.GetClientRect(h, ctypes.byref(r)); pt = wt.POINT(0, 0); U32.ClientToScreen(h, ctypes.byref(pt))
    W, H = r.right, r.bottom; w, hh = max(1, int(W * scale)), max(1, int(H * scale))
    sdc = U32.GetDC(0); mdc = G32.CreateCompatibleDC(sdc); bmp = G32.CreateCompatibleBitmap(sdc, w, hh)
    G32.SelectObject(mdc, bmp); G32.SetStretchBltMode(mdc, 4)
    G32.StretchBlt(mdc, 0, 0, w, hh, sdc, pt.x, pt.y, W, H, 0x00CC0020)
    class BIH(ctypes.Structure):
        _fields_ = [('biSize', wt.DWORD), ('biWidth', wt.LONG), ('biHeight', wt.LONG), ('biPlanes', wt.WORD),
                    ('biBitCount', wt.WORD), ('biCompression', wt.DWORD), ('biSizeImage', wt.DWORD),
                    ('a', wt.LONG), ('b', wt.LONG), ('c', wt.DWORD), ('d', wt.DWORD)]
    bi = BIH(ctypes.sizeof(BIH), w, -hh, 1, 24, 0, 0, 0, 0, 0, 0)
    stride = (w * 3 + 3) & ~3; buf = ctypes.create_string_buffer(stride * hh)
    G32.GetDIBits(mdc, bmp, 0, hh, buf, ctypes.byref(bi), 0)
    G32.DeleteObject(bmp); G32.DeleteDC(mdc); U32.ReleaseDC(0, sdc)
    rgb = bytearray()
    for y in range(hh):
        row = buf.raw[y * stride:y * stride + w * 3]
        rgb += bytes(b for i in range(0, len(row), 3) for b in (row[i + 2], row[i + 1], row[i]))
    png(path, w, hh, bytes(rgb)); return W, H

def sweep(seconds, speed=6, hz=250):
    """Smooth sinusoidal left-right camera sweep: mouse x only, never any key."""
    t0 = time.perf_counter(); acc = 0.0; n = 0
    while (t := time.perf_counter() - t0) < seconds:
        acc += speed * math.sin(2 * math.pi * t / 4.0)
        dx = int(acc); acc -= dx
        if dx: move(dx)
        n += 1; nxt = t0 + n / hz
        while time.perf_counter() < nxt: pass

def main():
    exe, cmd, *a = sys.argv[1:]
    pid = n3mon.find_pid(exe)
    if not pid: sys.exit(f'{exe} not running')
    h = find_window(pid)
    if not h: sys.exit('no window')
    if cmd == 'shot':
        print('client size', shot(h, a[0], float(a[1]) if len(a) > 1 else 0.5)); return
    focus(h)
    if cmd == 'focus': return
    if cmd == 'click': click(h, a[0], a[1])
    elif cmd == 'key': key(a[0], int(a[1]) if len(a) > 1 else 60)
    elif cmd == 'turn':
        for _ in range(int(a[1])):
            move(int(a[0])); time.sleep((int(a[2]) if len(a) > 2 else 10) / 1000)
    elif cmd == 'sweep': sweep(float(a[0]), float(a[1]) if len(a) > 1 else 6)
    else: sys.exit('unknown cmd')

if __name__ == '__main__':
    main()
