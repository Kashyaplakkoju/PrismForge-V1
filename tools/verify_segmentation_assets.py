"""Verify the exact app-local person model, contract, runtime and licensing assets."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODEL_HASH = "552d8a984054e59b5d773d24b9b12022b22046ceb2bbc4c9aaeaceb36a9ddf24"
RUNTIME_HASH = "903c92c54acc57caa77d44d4c856c829eb82f5cf6755b47367a8153bd3fabeb6"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(directory):
    assets = directory / "models" / "pphumanseg"
    checks = {assets / "model.onnx": MODEL_HASH,
              directory / "onnxruntime.dll": RUNTIME_HASH}
    for name, source in (("manifest.json", "pphumanseg-manifest.json"),
                         ("LICENSE", "PP-HumanSeg-LICENSE.txt"),
                         ("NOTICE", "PP-HumanSeg-NOTICE.txt")):
        checks[assets / name] = digest(ROOT / "tools" / "provider-deps" / source)
    notices = directory / "licenses" / "onnxruntime"
    for name in ("LICENSE", "ThirdPartyNotices.txt"):
        checks[notices / name] = digest(ROOT / "build" / "provider-deps" / "ort-sdk" / name)
    rows = []
    for path, expected in checks.items():
        actual = digest(path)
        if actual != expected:
            raise ValueError(f"Segmentation asset integrity mismatch: {path}")
        rows.append({"path": path.relative_to(directory).as_posix(), "sha256": actual})
    (directory / "SEGMENTATION-ASSETS.json").write_text(
        json.dumps({"model": "PP-HumanSeg2023mar", "runtime": "ORT1.20.1DirectML", "files": rows}, indent=2) + "\n",
        encoding="utf-8")
    print(f"PASS: {len(rows)} pinned segmentation assets and notices")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    verify(parser.parse_args().directory.resolve())
