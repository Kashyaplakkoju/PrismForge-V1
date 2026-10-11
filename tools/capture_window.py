import ctypes
from ctypes import wintypes
import sys
import time
from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
user32.SetProcessDPIAware()

def capture_window(out_filename="screenshot.png", wait_sec=1.0):
    time.sleep(wait_sec)
    hwnd = user32.FindWindowW(None, "VCamdroid Studio")
    if not hwnd:
        print("Window 'VCamdroid Studio' not found, searching with EnumWindows...")
        found_hwnds = []
        def enum_cb(h, l):
            t = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(h, t, 256)
            if "VCamdroid" in t.value and user32.IsWindowVisible(h):
                found_hwnds.append((h, t.value))
            return True
        WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
        user32.EnumWindows(WNDENUMPROC(enum_cb), 0)
        if found_hwnds:
            hwnd = found_hwnds[0][0]
            print(f"Found window: {found_hwnds[0][1]} (HWND {hwnd})")

    if not hwnd:
        print("No VCamdroid window found.")
        return False

    user32.ShowWindow(hwnd, 9) # SW_RESTORE
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.5)

    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w = rect.right - rect.left
    h = rect.bottom - rect.top

    if "--scroll" in sys.argv:
        pt_x = rect.left + int(w * 0.85)
        pt_y = rect.top + int(h * 0.5)
        user32.SetCursorPos(pt_x, pt_y)
        time.sleep(0.2)
        user32.mouse_event(0x0800, 0, 0, -600, 0)
        time.sleep(0.5)

    if w <= 0 or h <= 0:
        print(f"Invalid window size: {w}x{h}")
        return False

    hdc_screen = user32.GetDC(0)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_screen)
    hbmp = gdi32.CreateCompatibleBitmap(hdc_screen, w, h)
    gdi32.SelectObject(hdc_mem, hbmp)

    # Try PrintWindow with PW_RENDERFULLCONTENT (2)
    pw_ok = user32.PrintWindow(hwnd, hdc_mem, 2)
    if not pw_ok:
        # Fallback to regular PrintWindow or BitBlt
        if not user32.PrintWindow(hwnd, hdc_mem, 0):
            gdi32.BitBlt(hdc_mem, 0, 0, w, h, hdc_screen, rect.left, rect.top, 0x00CC0020)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [
            ('biSize', wintypes.DWORD),
            ('biWidth', wintypes.LONG),
            ('biHeight', wintypes.LONG),
            ('biPlanes', wintypes.WORD),
            ('biBitCount', wintypes.WORD),
            ('biCompression', wintypes.DWORD),
            ('biSizeImage', wintypes.DWORD),
            ('biXPelsPerMeter', wintypes.LONG),
            ('biYPelsPerMeter', wintypes.LONG),
            ('biClrUsed', wintypes.DWORD),
            ('biClrImportant', wintypes.DWORD)
        ]

    bmi = BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.biWidth = w
    bmi.biHeight = -h
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bmi.biCompression = 0

    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(hdc_mem, hbmp, 0, h, buf, ctypes.byref(bmi), 0)

    img = Image.frombuffer('RGBA', (w, h), buf, 'raw', 'BGRA', 0, 1)
    r, g, b, a = img.split()
    img_rgb = Image.merge('RGB', (r, g, b))
    img_rgb.save(out_filename)
    print(f"Captured {w}x{h} to {out_filename}")

    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(0, hdc_screen)
    return True

if __name__ == "__main__":
    out_file = sys.argv[1] if len(sys.argv) > 1 else "screenshot.png"
    capture_window(out_file)
