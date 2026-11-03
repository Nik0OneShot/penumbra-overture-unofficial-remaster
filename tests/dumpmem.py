# Read-only dump of the running Penumbra.exe image (after the Steam stub has decrypted .text).
# Usage (game running at the main menu): python -I tests\dumpmem.py <out.bin>
import ctypes, ctypes.wintypes as wt, subprocess, sys

PROCESS_VM_READ = 0x10
PROCESS_QUERY_INFORMATION = 0x400
IMAGE_BASE = 0x400000
IMAGE_SIZE = 0x310000  # SizeOfImage rounded up; covers .text .rdata .data .rsrc .bind

out = subprocess.check_output(['tasklist', '/FI', 'IMAGENAME eq Penumbra.exe', '/FO', 'CSV', '/NH'], text=True)
pids = [int(l.split('","')[1]) for l in out.strip().splitlines() if l.startswith('"Penumbra.exe"')]
if not pids:
    sys.exit("Penumbra.exe is not running")
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pids[0])
if not h:
    sys.exit("OpenProcess failed: %d" % ctypes.get_last_error())

img = bytearray(IMAGE_SIZE)
page = 0x1000
buf = ctypes.create_string_buffer(page)
n = ctypes.c_size_t()
ok = 0
for off in range(0, IMAGE_SIZE, page):
    if k32.ReadProcessMemory(h, ctypes.c_void_p(IMAGE_BASE + off), buf, page, ctypes.byref(n)):
        img[off:off + page] = buf.raw
        ok += 1
k32.CloseHandle(h)
open(sys.argv[1], 'wb').write(img)
print("pid %d: read %d/%d pages -> %s" % (pids[0], ok, IMAGE_SIZE // page, sys.argv[1]))
