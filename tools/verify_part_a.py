import ctypes
from ctypes import wintypes
import sys
import time
from PIL import Image, ImageChops

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
user32.SetProcessDPIAware()

def get_window_rect(hwnd):
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return rect

def capture_window_image(hwnd):
    user32.SetCursorPos(0, 0)
    time.sleep(0.8) # Allow scrollbar thumb fade timer to settle to 0
    rect = get_window_rect(hwnd)
    w = rect.right - rect.left
    h = rect.bottom - rect.top
    if w <= 0 or h <= 0:
        return None

    hdc_screen = user32.GetDC(0)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_screen)
    hbmp = gdi32.CreateCompatibleBitmap(hdc_screen, w, h)
    gdi32.SelectObject(hdc_mem, hbmp)

    user32.PrintWindow(hwnd, hdc_mem, 2)

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

    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(0, hdc_screen)
    return img_rgb

def trigger_force_full_repaint(hwnd):
    # Send WM_COMMAND with DEBUG_FORCE_REPAINT (238)
    user32.SendMessageW(hwnd, 0x0111, 238, 0)
    time.sleep(0.5)

def check_pixel_identical(img_before, img_after, test_name):
    diff = ImageChops.difference(img_before, img_after)
    bbox = diff.getbbox()
    if bbox is None:
        print(f"[PASS] {test_name}: PIXEL IDENTICAL before and after Force Full Repaint")
        return True
    else:
        print(f"[NOTE] {test_name}: Difference bbox in client area: {bbox}")
        w = img_before.width
        insp1 = img_before.crop((int(w * 0.65), 0, img_before.width, img_before.height))
        insp2 = img_after.crop((int(w * 0.65), 0, img_after.width, img_after.height))
        insp_diff = ImageChops.difference(insp1, insp2)
        print(f"       Inspector area diff bbox: {insp_diff.getbbox()}")
        return False


def find_vcamdroid_window():
    hwnd = user32.FindWindowW(None, "VCamdroid Studio")
    return hwnd

import os
import subprocess

if __name__ == "__main__":
    proc = None
    hwnd = find_vcamdroid_window()
    if not hwnd:
        print("VCamdroid Studio window not found, launching...")
        env = os.environ.copy()
        env["VCAMDROID_UI_PREVIEW"] = "1"
        exe_path = os.path.abspath(r"windows\x64\Release\VCamdroid.exe")
        work_dir = os.path.abspath(r"windows\x64\Release")
        proc = subprocess.Popen([exe_path], cwd=work_dir, env=env)
        for _ in range(20):
            time.sleep(0.5)
            hwnd = find_vcamdroid_window()
            if hwnd:
                break

    if not hwnd:
        print("Failed to find or launch VCamdroid Studio window!")
        if proc: proc.terminate()
        sys.exit(1)

    print(f"Found window HWND: {hwnd}")
    user32.ShowWindow(hwnd, 9)
    rect = get_window_rect(hwnd)
    user32.SetWindowPos(hwnd, 0, rect.left, rect.top, 1440, 900, 0x0004)
    time.sleep(1.2)

    tests_passed = 0
    total_tests = 0

    # Test 1: Initial state
    total_tests += 1
    before = capture_window_image(hwnd)
    before.save("part_a_test1_before_repaint.png")
    trigger_force_full_repaint(hwnd)
    after = capture_window_image(hwnd)
    after.save("part_a_test1_after_repaint.png")
    if check_pixel_identical(before, after, "Test 1 (Initial state)"):
        tests_passed += 1

    # Test 2: Scroll down to bottom
    total_tests += 1
    rect = get_window_rect(hwnd)
    w = rect.right - rect.left
    h = rect.bottom - rect.top
    insp_x = rect.left + int(w * 0.85)
    insp_y = rect.top + int(h * 0.5)
    user32.SetCursorPos(insp_x, insp_y)
    time.sleep(0.2)
    # Scroll down 15 clicks
    for _ in range(15):
        user32.mouse_event(0x0800, 0, 0, -240, 0)
        time.sleep(0.05)
    time.sleep(1.2)
    before = capture_window_image(hwnd)
    before.save("part_a_test2_scrolled_bottom_before.png")
    trigger_force_full_repaint(hwnd)
    after = capture_window_image(hwnd)
    after.save("part_a_test2_scrolled_bottom_after.png")
    if check_pixel_identical(before, after, "Test 2 (Scrolled to bottom)"):
        tests_passed += 1

    # Test 3: Collapse section while scrolled to bottom
    total_tests += 1
    # Click near top of inspector to collapse a visible section header
    user32.SetCursorPos(insp_x, rect.top + int(h * 0.35))
    time.sleep(0.1)
    user32.mouse_event(0x0002, 0, 0, 0, 0) # left down
    user32.mouse_event(0x0004, 0, 0, 0, 0) # left up
    time.sleep(1.2)
    before = capture_window_image(hwnd)
    before.save("part_a_test3_collapse_while_scrolled_before.png")
    trigger_force_full_repaint(hwnd)
    after = capture_window_image(hwnd)
    after.save("part_a_test3_collapse_while_scrolled_after.png")
    if check_pixel_identical(before, after, "Test 3 (Collapse while scrolled)"):
        tests_passed += 1

    # Test 4: Window height 600
    total_tests += 1
    user32.SetWindowPos(hwnd, 0, rect.left, rect.top, 1440, 600, 0x0004) # SWP_NOZORDER
    time.sleep(1.2)
    before = capture_window_image(hwnd)
    before.save("part_a_test4_h600_before.png")
    trigger_force_full_repaint(hwnd)
    after = capture_window_image(hwnd)
    after.save("part_a_test4_h600_after.png")
    if check_pixel_identical(before, after, "Test 4 (Height 600)"):
        tests_passed += 1

    # Test 5: Window height 800
    total_tests += 1
    user32.SetWindowPos(hwnd, 0, rect.left, rect.top, 1440, 800, 0x0004)
    time.sleep(1.2)
    before = capture_window_image(hwnd)
    before.save("part_a_test5_h800_before.png")
    trigger_force_full_repaint(hwnd)
    after = capture_window_image(hwnd)
    after.save("part_a_test5_h800_after.png")
    if check_pixel_identical(before, after, "Test 5 (Height 800)"):
        tests_passed += 1

    # Test 6: Window height 1080
    total_tests += 1
    user32.SetWindowPos(hwnd, 0, rect.left, rect.top, 1440, 1080, 0x0004)
    time.sleep(1.2)
    before = capture_window_image(hwnd)
    before.save("part_a_test6_h1080_before.png")
    trigger_force_full_repaint(hwnd)
    after = capture_window_image(hwnd)
    after.save("part_a_test6_h1080_after.png")
    if check_pixel_identical(before, after, "Test 6 (Height 1080)"):
        tests_passed += 1

    print(f"\nResult: {tests_passed}/{total_tests} tests verified PIXEL-IDENTICAL!")
    if proc:
        time.sleep(0.5)
        proc.terminate()
