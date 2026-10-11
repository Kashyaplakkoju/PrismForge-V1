"""Check application DEX call sites, not source, before sharing/installing an APK.

Usage: python tools/check_apk_rotation.py app.apk --dexdump SDK/build-tools/36.0.0/dexdump.exe
"""
import argparse
import hashlib
import pathlib
import re
import subprocess
import tempfile
import zipfile


def check(apk: pathlib.Path, dexdump: pathlib.Path) -> bool:
    calls = []
    with tempfile.TemporaryDirectory(prefix="vcamdroid-dex-") as temporary, zipfile.ZipFile(apk) as archive:
        for name in archive.namelist():
            if not re.fullmatch(r"classes\d*\.dex", name):
                continue
            path = pathlib.Path(temporary) / name
            path.write_bytes(archive.read(name))
            dump = subprocess.check_output([str(dexdump), "-d", str(path)], text=True, encoding="utf-8", errors="replace")
            for block in re.split(r"(?m)^Class #\d+", dump):
                descriptor = re.search(r"Class descriptor\s*:\s*'([^']+)'", block)
                if not descriptor or not descriptor[1].startswith("Lcom/darusc/vcamdroid/"):
                    continue
                for call in re.findall(r"[^\n]*invoke-[^\n]*\.prepareVideo:\(I+\)Z[^\n]*", block):
                    calls.append((descriptor[1], call.strip()))
    print("APK SHA-256:", hashlib.sha256(apk.read_bytes()).hexdigest())
    for owner, call in calls:
        print(owner, call)
    wrong = [call for _, call in calls if ".prepareVideo:(IIIII)Z" in call]
    correct = [call for _, call in calls if ".prepareVideo:(IIIIII)Z" in call]
    if wrong or not correct:
        print("FAIL: five-argument encoder call found or six-argument call missing")
        return False
    print("PASS: application uses the six-argument encoder overload")
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=pathlib.Path)
    parser.add_argument("--dexdump", type=pathlib.Path, required=True)
    args = parser.parse_args()
    raise SystemExit(0 if check(args.apk, args.dexdump) else 1)
