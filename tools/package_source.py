"""Create a verified, source-only ZIP without modifying the checkout. Python 3.9+."""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
EXCLUDE_DIRS = {'.git', '.codex', '.agents', '.vs', '.idea', '.vscode', '.gradle', '.cxx',
                'build', 'dist', 'node_modules', '.cache', '__pycache__', 'scratch', 'x64', 'Debug', 'Release'}
EXCLUDE_SUFFIXES = {'.exe', '.dll', '.lib', '.obj', '.exp', '.pdb', '.aps', '.apk', '.aab',
                    '.zip', '.log', '.tmp', '.bak', '.user', '.suo', '.pyc', '.jks', '.keystore',
                    '.p12', '.pfx', '.pem', '.key', '.o', '.tlog', '.ilk', '.idb', '.pch'}
EXCLUDE_NAMES = {'.clangd', '.gitmodules', 'ProjectGPTExport.md', 'local.properties', 'key.properties',
                 'keystore.properties', 'credentials.json', 'settings.cfg', 'studio-presets.ini'}

def excluded(name):
    path = PurePosixPath(name)
    return (any(part in EXCLUDE_DIRS for part in path.parts) or path.suffix.lower() in EXCLUDE_SUFFIXES
            or path.name in EXCLUDE_NAMES or path.name.startswith('.env')
            or name.startswith('.github/upgrades/') or name == 'imgs/demo.gif'
            or name == 'docs/CAMERA-TILT-INVESTIGATION.md') and not name.startswith('assets/starter-packs/') and not name.startswith('assets/starter-packs/')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'dist/share/PrismForgeStudio-V1-source.zip')
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        raw = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=ROOT)
        names = sorted(set(p.decode('utf-8') for p in raw.split(b'\0') if p))
    except (OSError, subprocess.CalledProcessError):
        names = sorted(p.relative_to(ROOT).as_posix() for p in ROOT.rglob('*') if p.is_file())
    payload = {}
    for name in names:
        source = ROOT / name
        if excluded(name) or not source.is_file():
            continue
        if not source.resolve().is_relative_to(ROOT):
            raise ValueError(f'External file refused: {name}')
        data = source.read_bytes()
        if len(data) > (40 if source.suffix.lower() in ('.pfmodel', '.pfrgba') else 5) * 1024 * 1024:
            raise ValueError(f'Unexpected large source file; review before sharing: {name}')
        # Never print possible secret values.
        for pattern in (rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
                        rb'\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})\b',
                        rb'\bAKIA[0-9A-Z]{16}\b', rb'\bAIza[0-9A-Za-z_-]{35}\b',
                        rb'\bsk-(?:proj-)?[A-Za-z0-9_-]{32,}\b'):
            if re.search(pattern, data):
                raise ValueError(f'Possible credential found; review before sharing: {name}')
        payload[name] = data
    required = {'README.md', 'LICENSE', 'android/gradle/wrapper/gradle-wrapper.jar',
                'windows/src/mfsource/MediaSource.cpp', 'windows/3rdparty/softcam/LICENSE'}
    if not required.issubset(payload):
        raise ValueError('Required source files are missing')
    manifest = {p: {'bytes': len(d), 'sha256': hashlib.sha256(d).hexdigest()} for p, d in payload.items()}
    payload['PACKAGE-MANIFEST.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(payload.items()):
            info = zipfile.ZipInfo('PrismForgeStudio-V1/' + name)
            info.create_system = 3
            info.external_attr = (0o100755 if name == 'android/gradlew' else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    with zipfile.ZipFile(output) as archive:
        assert archive.testzip() is None
        for name, row in manifest.items():
            assert hashlib.sha256(archive.read('PrismForgeStudio-V1/' + name)).hexdigest() == row['sha256']
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(output.suffix + '.sha256').write_text(f'{digest}  {output.name}\n', encoding='utf-8')
    print(f'Verified {len(payload)} files; {output.stat().st_size:,} bytes: {output}')

if __name__ == '__main__':
    main()
