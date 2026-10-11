"""Package verified source and ready-to-use apps. Never installs or deletes anything.

Requires Python 3.10+, existing release builds, and Windows developer dependencies.
Recipients do not need developer tools. See docs/DISTRIBUTION.md and --help.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def required(path):
    path = Path(path)
    if not path.is_file():
        raise FileNotFoundError(f'Required package input missing: {path}')
    return path


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def audit_imports(files, dumpbin):
    """Check architecture, normal/delay imports, and app-local CRT coverage."""
    names = {name.lower(): name for name in files}
    def machine(path):
        data = path.read_bytes()
        offset = struct.unpack_from('<I', data, 0x3C)[0]
        if data[:2] != b'MZ' or data[offset:offset + 4] != b'PE\0\0':
            raise ValueError(f'Invalid PE: {path.name}')
        return struct.unpack_from('<H', data, offset + 4)[0]
    adb_machine = machine(files['Windows/adb/adb.exe'])
    if adb_machine not in (0x14C, 0x8664):
        raise ValueError('ADB must be an x86 or x64 Windows executable')
    report = {}
    for name, path in sorted(files.items()):
        if not name.startswith('Windows/') or path.suffix.lower() not in ('.dll', '.exe'):
            continue
        # The signed x64 runtime installer itself uses a 32-bit bootstrap executable.
        if name == 'Windows/Runtime/vc_redist.x64.exe':
            continue
        adb = name.startswith('Windows/adb/')
        expected_machine = adb_machine if adb else 0x8664
        if machine(path) != expected_machine:
            raise ValueError(f'Incompatible PE architecture: {name}')
        system = Path(os.environ['SystemRoot']) / ('SysWOW64' if expected_machine == 0x14C else 'System32')
        output = subprocess.check_output([str(dumpbin), '/nologo', '/dependents', str(path)], text=True)
        imports = sorted(set(re.findall(r'^\s+([\w.+-]+\.dll)\s*$', output, re.M | re.I)), key=str.lower)
        if not imports:
            raise ValueError(f'No imports parsed for {name}')
        resolved = {}
        for dep in imports:
            local = (Path(name).parent / dep).as_posix().lower()
            app = ('Windows/' + dep).lower()
            match = names.get(local) or (names.get(app) if not adb else None)
            if match:
                resolved[dep] = match
            elif dep.lower().startswith(('api-ms-', 'ext-ms-')):
                resolved[dep] = 'Windows OS API contract'
            elif re.match(r'(?i)(msvcp|vcruntime|concrt|vccorlib)', dep):
                raise ValueError(f'Missing bundled Microsoft runtime: {name} -> {dep}')
            elif (system / dep).is_file():
                resolved[dep] = 'Windows OS component'
            else:
                raise ValueError(f'Missing runtime dependency: {name} -> {dep}')
        report[name] = {'architecture': 'x86' if expected_machine == 0x14C else 'x64', 'imports': resolved}
    return report


def write_zip(output, payload):
    manifest = {name: {'bytes': len(data), 'sha256': sha256(data)} for name, data in sorted(payload.items())}
    contents = dict(payload)
    contents['PACKAGE-MANIFEST.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, data in sorted(contents.items()):
            info = zipfile.ZipInfo('VCamdroid/' + name)
            info.create_system = 3
            info.external_attr = (0o100755 if name.endswith('/gradlew') else 0o100644) << 16
            info.compress_type = zipfile.ZIP_STORED if name.endswith(('.apk', '.tar.gz')) else zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None or len(archive.namelist()) != len(contents):
            raise ValueError('ZIP integrity verification failed')
        for name, row in manifest.items():
            if sha256(archive.read('VCamdroid/' + name)) != row['sha256']:
                raise ValueError(f'ZIP hash mismatch: {name}')
    output.with_suffix('.zip.sha256').write_text(f'{sha256(output.read_bytes())}  {output.name}\n', encoding='utf-8')
    print(f'Verified {len(contents)} entries: {output.name} ({output.stat().st_size:,} bytes)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--windows-build', type=Path, default=ROOT / 'windows/x64/Release')
    parser.add_argument('--camera-build', type=Path, default=ROOT / 'dist')
    parser.add_argument('--softcam', type=Path, default=ROOT / 'windows/3rdparty/softcam/dist/bin/x64/softcam.dll')
    parser.add_argument('--apk', type=Path, default=ROOT / 'android/app/build/outputs/apk/release/app-release.apk')
    parser.add_argument('--vcpkg-root', type=Path, default=Path('C:/vcpkg'))
    parser.add_argument('--platform-tools', type=Path, default=Path('C:/Android_studio/platform-tools'))
    parser.add_argument('--vs-root', type=Path)
    parser.add_argument('--date', default=datetime.date.today().isoformat())
    parser.add_argument('--binary-source-commit', required=True, help='Commit actually used to compile the Windows EXE')
    parser.add_argument('--android-source-commit', help='APK build commit when reusing an unchanged Android build')
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'dist/share')
    args = parser.parse_args()
    if not re.fullmatch(r'\d{4}-\d{2}-\d{2}', args.date):
        parser.error('--date must be YYYY-MM-DD')
    if args.vs_root:
        vs = args.vs_root
    else:
        where = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        vs = Path(subprocess.check_output([str(required(where)), '-latest', '-products', '*', '-requires',
            'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip())
    crt = sorted((vs / 'VC/Redist/MSVC').glob('*/x64/Microsoft.VC*.CRT'))[-1]
    dumpbin = required(sorted((vs / 'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/dumpbin.exe'))[-1])
    redist = required(crt.parents[1] / 'vc_redist.x64.exe')
    # Pass the path through the child's environment; do not interpolate shell code.
    environment = dict(os.environ, VCAM_PACKAGE_REDIST=str(redist))
    subprocess.run([shutil.which('pwsh') or 'powershell', '-NoProfile', '-NonInteractive', '-Command',
        '$s=Get-AuthenticodeSignature -LiteralPath $env:VCAM_PACKAGE_REDIST; '
        'if($s.Status -ne "Valid" -or $s.SignerCertificate.Subject -notmatch "O=Microsoft Corporation") {exit 1}'],
        env=environment, check=True)
    files = {}

    def add(name, path):
        if name in files:
            raise ValueError(f'Duplicate package entry: {name}')
        files[name] = required(path)

    add('Windows/VCamdroid.exe', args.windows_build / 'VCamdroid.exe')
    for path in args.windows_build.glob('*.dll'):
        provenance = required(args.vcpkg_root / 'installed/x64-windows/bin' / path.name)
        if sha256(path.read_bytes()) != sha256(provenance.read_bytes()):
            raise ValueError(f'Runtime DLL differs from licensed installed vcpkg build: {path.name}')
        add('Windows/' + path.name, path)
    for name in ('vcam_installer.exe', 'vcamdroid_source.dll'):
        add('Windows/' + name, args.camera_build / name)
    add('Windows/softcam.dll', args.softcam)
    for path in crt.glob('*.dll'):
        add('Windows/' + path.name, path)
    add('Windows/Runtime/vc_redist.x64.exe', redist)
    for path in (ROOT / 'windows/res').iterdir():
        if path.suffix.lower() in ('.png', '.ico'):
            add('Windows/res/' + path.name, path)
    for name in ('adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll'):
        add('Windows/adb/' + name, args.platform_tools / name)
    add('Android/VCamdroid.apk', args.apk)
    add('LICENSE', ROOT / 'LICENSE')
    add('ThirdParty/licenses/Softcam.txt', ROOT / 'windows/3rdparty/softcam/LICENSE')
    add('ThirdParty/licenses/QR-Code-generator.txt', ROOT / 'QR-Code-generator/Readme.markdown')
    add('ThirdParty/licenses/Android-Platform-Tools-NOTICE.txt', args.platform_tools / 'NOTICE.txt')
    for name in ('RootEncoder-2.6.1.txt', 'RTSP-Server-1.3.6.txt'):
        add('ThirdParty/licenses/' + name, ROOT / 'docs/licenses' / name)
    for name in ('Redist.txt', 'ThirdPartyNotices.txt'):
        add('ThirdParty/licenses/Microsoft-' + name, vs / 'Licenses/1033' / name)
    share = args.vcpkg_root / 'installed/x64-windows/share'
    dependencies = {}
    for package in ('ffmpeg', 'wxwidgets', 'asio', 'zlib', 'libjpeg-turbo', 'libpng', 'liblzma', 'libwebp', 'tiff', 'pcre2', 'nanosvg', 'expat'):
        add('ThirdParty/licenses/' + package + '.txt', share / package / 'copyright')
        dependencies[package] = json.loads(required(share / package / 'vcpkg.spdx.json').read_text(encoding='utf-8'))['name']
    # Include corresponding LGPL source and matching build patches alongside the DLLs.
    port = args.vcpkg_root / 'ports/ffmpeg'
    version = json.loads(required(port / 'vcpkg.json').read_text())['version']
    source = required(args.vcpkg_root / 'downloads' / f'ffmpeg-ffmpeg-n{version}.tar.gz')
    expected = re.search(r'SHA512\s+([a-f0-9]{128})', required(port / 'portfile.cmake').read_text()).group(1)
    if hashlib.sha512(source.read_bytes()).hexdigest() != expected:
        raise ValueError('FFmpeg source archive does not match the included build recipe')
    add('ThirdParty/FFmpeg/source.tar.gz', source)
    for path in port.iterdir():
        if path.is_file():
            add('ThirdParty/FFmpeg/vcpkg-port/' + path.name, path)
    imports = audit_imports(files, dumpbin)
    payload = {name: path.read_bytes() for name, path in files.items()}
    documents = ROOT / 'docs/distribution'
    for name, path in [('README.md', documents / 'README.md'),
                       ('START-HERE.txt', documents / 'START-HERE.txt'),
                       ('ThirdParty/README.txt', documents / 'THIRD-PARTY.txt'),
                       ('VALIDATION.txt', documents / 'VALIDATION.txt')]:
        payload[name] = required(path).read_bytes()
    for name in ('Start VCamdroid.cmd', 'Setup Camera.cmd', 'Remove Camera.cmd'):
        text = required(documents / name).read_text(encoding='utf-8')
        payload['Windows/' + name] = text.replace('\n', '\r\n').encode()
    payload['DEPENDENCY-AUDIT.json'] = (json.dumps(imports, indent=2) + '\n').encode()
    info = {'packaged_on': args.date,
        'packaging_source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'packaging_checkout_modified': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT)),
        'binary_source_commit': args.binary_source_commit,
        'windows_binary_source_commit': args.binary_source_commit,
        'android_binary_source_commit': args.android_source_commit or args.binary_source_commit,
        'android_variant': 'release',
        'android_signing': 'local debug certificate; private key not distributed',
        'android_sha256': sha256(payload['Android/VCamdroid.apk']),
        'windows_exe_sha256': sha256(payload['Windows/VCamdroid.exe']),
        'dependencies': dependencies, 'camera_setup_executed_during_packaging': False}
    payload['BUILD-INFO.json'] = (json.dumps(info, indent=2) + '\n').encode()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    source_zip = args.output_dir / f'VCamdroid-Source-{args.date}.zip'
    subprocess.run([sys.executable, str(ROOT / 'tools/package_source.py'), '--output', str(source_zip)], check=True)
    write_zip(args.output_dir / f'VCamdroid-Ready-to-use-{args.date}.zip', payload)
    with zipfile.ZipFile(source_zip) as archive:
        for name in archive.namelist():
            payload['Source/' + name.removeprefix('VCamdroid/')] = archive.read(name)
    write_zip(args.output_dir / f'VCamdroid-Complete-{args.date}.zip', payload)


if __name__ == '__main__':
    main()
