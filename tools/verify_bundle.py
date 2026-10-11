"""Verify and extract an app bundle; optionally load DLLs and start isolated UI preview.

Never runs camera setup. Windows loader validation requires 64-bit Python.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--extract', type=Path, required=True, help='New, empty verification directory')
    parser.add_argument('--launch-preview', action='store_true')
    args = parser.parse_args()
    if args.extract.exists() and any(args.extract.iterdir()):
        raise ValueError('Use a new or empty extraction directory; existing files are never removed')
    with zipfile.ZipFile(args.archive) as archive:
        if archive.testzip() is not None:
            raise ValueError('Corrupt ZIP')
        members = archive.namelist()
        if len(members) != len(set(members)):
            raise ValueError('Duplicate ZIP entries')
        for name in members:
            path = PurePosixPath(name)
            if path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name:
                raise ValueError('Unsafe ZIP member path')
        manifest = json.loads(archive.read('VCamdroid/PACKAGE-MANIFEST.json'))
        if set(members) != {'VCamdroid/' + name for name in manifest} | {'VCamdroid/PACKAGE-MANIFEST.json'}:
            raise ValueError('Manifest does not cover exactly the ZIP payload')
        for name, row in manifest.items():
            data = archive.read('VCamdroid/' + name)
            if len(data) != row['bytes'] or hashlib.sha256(data).hexdigest() != row['sha256']:
                raise ValueError(f'Payload mismatch: {name}')
        forbidden = {'settings.cfg', 'studio-presets.ini', 'local.properties', '.git', '.gitmodules'}
        for name in manifest:
            parts = PurePosixPath(name).parts
            if any(part in forbidden for part in parts) or name.endswith(('.pdb', '.jks', '.keystore', '.key')):
                raise ValueError(f'Private/generated file found: {name}')
        archive.extractall(args.extract)
    windows = (args.extract / 'VCamdroid/Windows').resolve()
    environment = dict(os.environ)
    environment['PATH'] = str(Path(os.environ['SystemRoot']) / 'System32')
    environment['VCAMDROID_UI_PREVIEW'] = '1'
    os.environ['PATH'] = environment['PATH']
    os.chdir(windows)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
    kernel.LoadLibraryExW.restype = ctypes.c_void_p
    kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
    handles = []
    for path in sorted(windows.glob('*.dll')):
        # Dependencies can resolve only beside this DLL or in Windows System32.
        handle = kernel.LoadLibraryExW(str(path), None, 0x100 | 0x800)
        if not handle:
            raise OSError(ctypes.get_last_error(), f'Isolated DLL load failed: {path.name}')
        handles.append(handle)
    print(f'PASS: ZIP manifest and isolated System32/app-folder loads for {len(handles)} DLLs')
    output = subprocess.check_output([str(windows / 'adb/adb.exe'), 'version'], env=environment, text=True)
    print(output.splitlines()[0])
    for handle in reversed(handles):
        kernel.FreeLibrary(handle)
    if args.launch_preview:
        child = subprocess.Popen([str(windows / 'VCamdroid.exe')], cwd=windows,
                                 env=environment, creationflags=subprocess.CREATE_NO_WINDOW)
        print(f'Isolated preview PID {child.pid}; no networking, ADB server or camera registration started')


if __name__ == '__main__':
    main()
