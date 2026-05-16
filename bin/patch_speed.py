"""
patch_speed.py -- Remove realtime throttle from SWIttsEngine.dll

The Speechify engine calls Sleep() after producing each audio chunk,
throttling synthesis to match realtime playback speed. This is useless
for file output and adds 35+ seconds to a 200-word synthesis.

The patch NOPs a single PUSH+CALL sequence (7 bytes at file offset
0x123DA) that calls kernel32.Sleep with the throttle duration.

Usage:
    1. Stop Speechify.exe
    2. python patch_speed.py
    3. Restart Speechify.exe
    4. Enjoy 14x faster synthesis

To restore: copy SWIttsEngine_orig.dll over SWIttsEngine.dll
"""
import os
import shutil
import struct
import sys

import psutil

def kill_speechify():
    """Kill any running Speechify process to free file locks."""
    for proc in psutil.process_iter(['pid', 'name']):
        if proc.info['name'] and proc.info['name'].lower() in ("speechify.exe", "speechify"):
            proc.kill()
            proc.wait(timeout=5)
            print("Killed Speechify process (pid %d) to free file locks." % proc.info['pid'])

DLL = 'SWIttsEngine.dll'
BACKUP = 'SWIttsEngine_orig.dll'

# Patch site: PUSH ESI + CALL [IAT_Sleep] at file offset 0x123DA
PATCH_OFFSET = 0x123DA
ORIG_BYTES = bytes.fromhex('56ff1524f0b106')  # PUSH ESI; CALL [06B1F024]
NOP_BYTES  = b'\x90' * 7                       # 7x NOP

def main():
    kill_speechify()

    if not os.path.exists(DLL):
        print('ERROR: %s not found (run from bin/ directory)' % DLL)
        return 1

    with open(DLL, 'rb') as f:
        data = bytearray(f.read())

    # Verify we're patching the right bytes
    actual = bytes(data[PATCH_OFFSET:PATCH_OFFSET + 7])
    if actual == NOP_BYTES:
        print('Already patched!')
        return 0
    if actual != ORIG_BYTES:
        print('ERROR: unexpected bytes at 0x%X' % PATCH_OFFSET)
        print('  Expected: %s' % ORIG_BYTES.hex())
        print('  Found:    %s' % actual.hex())
        print('  DLL may be a different version.')
        return 1

    # Backup
    if not os.path.exists(BACKUP):
        shutil.copy2(DLL, BACKUP)
        print('Backup: %s' % BACKUP)
    else:
        print('Backup already exists: %s' % BACKUP)

    # Patch
    data[PATCH_OFFSET:PATCH_OFFSET + 7] = NOP_BYTES

    with open(DLL, 'wb') as f:
        f.write(data)

    print('Patched %s: Sleep throttle removed (7 bytes NOPed at 0x%X)' %
          (DLL, PATCH_OFFSET))
    print('Synthesis should now be ~14x faster for batch output.')
    print('To restore: copy %s over %s' % (BACKUP, DLL))
    return 0

if __name__ == '__main__':
    sys.exit(main())
