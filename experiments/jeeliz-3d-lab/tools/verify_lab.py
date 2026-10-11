#!/usr/bin/env python3
"""Verify pinned files, real geometry, script syntax, and actual loopback HTTP."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
from html.parser import HTMLParser
import http.client
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading

sys.dont_write_bytecode = True
LAB = Path(__file__).resolve().parents[1]
SITE = LAB / "site"
EXPECTED_COMMIT = "6f2695120b992511fd6cb1fd80c600bb957cd08c"
THREE_LICENSE_COMMIT = "2410ad1a8f5844df00191600eafda59def537830"
RUNTIME_SCRIPTS = ("vendor/three.js", "vendor/jeelizFaceFilter.js", "vendor/JeelizThreeHelper.js",
                   "vendor/GLTFLoader.js", "local-model.js", "model-authoring.js", "head-attachment.js", "dog-rig.js", "wildlife-rigs.js", "original-rigs.js", "pose.js", "filter-rigs.js", "lab.js")


class Checks:
    def __init__(self):
        self.count = 0
        self.failures = []
        self.commands = []

    def ensure(self, condition, message):
        self.count += 1
        if not condition:
            self.failures.append(message)
            raise AssertionError(message)

    def run(self, arguments):
        environment = os.environ.copy()
        environment["PYTHONDONTWRITEBYTECODE"] = "1"
        result = subprocess.run(arguments, cwd=LAB, capture_output=True, text=True, timeout=40, env=environment)
        record = {"command": arguments, "exitCode": result.returncode,
                  "stdout": result.stdout.strip(), "stderr": result.stderr.strip()}
        self.commands.append(record)
        self.ensure(result.returncode == 0, "Child command failed: " + " ".join(arguments) + "\n" + result.stderr)
        return record


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def local_file(relative):
    if not isinstance(relative, str) or "\\" in relative or ":" in relative or relative.startswith("/"):
        raise ValueError("Invalid site path: " + str(relative))
    if any(part in ("", ".", "..") for part in relative.split("/")):
        raise ValueError("Invalid site path components: " + relative)
    path = (SITE / relative).resolve()
    if not path.is_relative_to(SITE.resolve()):
        raise ValueError("Site path escapes lab")
    return path


def numeric_array(checks, values, message):
    checks.ensure(isinstance(values, list) and len(values) > 0, message + " has data")
    checks.ensure(all(type(value) in (int, float) and math.isfinite(value) for value in values), message + " is finite numeric data")


def check_manifest(checks):
    manifest = load_json(SITE / "assets-manifest.json")
    checks.ensure(manifest.get("schemaVersion") == 1, "Manifest schema version")
    checks.ensure(manifest.get("upstream", {}).get("repository") == "https://github.com/jeeliz/jeelizFaceFilter", "Manifest upstream repository")
    checks.ensure(manifest["upstream"].get("commit") == EXPECTED_COMMIT, "Exact pinned Jeeliz commit")
    assets = manifest.get("assets")
    checks.ensure(isinstance(assets, list) and len(assets) >= 38, "All 38 required pinned collection asset records")
    seen = set()
    roles = {}
    for record in assets:
        relative = record.get("path")
        checks.ensure(relative not in seen, "Unique pinned asset path " + str(relative))
        seen.add(relative)
        path = local_file(relative)
        checks.ensure(path.is_file(), "Pinned asset exists " + relative)
        checks.ensure(re.fullmatch(r"[0-9a-fA-F]{64}", str(record.get("sha256", ""))) is not None, "SHA-256 syntax " + relative)
        checks.ensure(type(record.get("bytes")) is int and record["bytes"] > 0, "Declared byte size " + relative)
        checks.ensure(path.stat().st_size == record["bytes"], "Pinned byte size " + relative)
        checks.ensure(digest(path) == record["sha256"].lower(), "Pinned SHA-256 " + relative)
        source = record.get("sourceUrl", "")
        if relative in ("vendor/THREE-LICENSE.txt", "vendor/GLTFLoader.js"):
            prefix = "https://raw.githubusercontent.com/mrdoob/three.js/" + THREE_LICENSE_COMMIT + "/"
            checks.ensure(record.get("upstreamCommit") == THREE_LICENSE_COMMIT, "Pinned Three license commit")
        else:
            prefix = "https://raw.githubusercontent.com/jeeliz/jeelizFaceFilter/" + EXPECTED_COMMIT + "/"
        checks.ensure(source.startswith(prefix), "Pinned immutable source URL " + relative)
        checks.ensure(source == prefix + record.get("upstreamPath", ""), "Source URL matches upstream path " + relative)
        roles[record.get("role")] = roles.get(record.get("role"), 0) + 1
    checks.ensure(roles.get("script") == 4, "Four vendored script records")
    checks.ensure(roles.get("geometry", 0) >= 9, "All nine genuine geometry records")
    checks.ensure(roles.get("texture", 0) >= 18, "All eighteen local texture records")
    checks.ensure(roles.get("model") == 1 and roles.get("license") == 2, "Model and preserved license records")
    checks.ensure(all(path in seen for path in RUNTIME_SCRIPTS[:3]), "Required vendored runtime libraries")
    required_geometry = {"assets/dog/dog_ears.json", "assets/dog/dog_nose.json", "assets/dog/dog_tongue.json",
                         "assets/helmet/helmet.json", "assets/helmet/visiere.json", "assets/helmet/face.json", "assets/mask/mask.json",
                         "assets/tiger/TigerHead.json", "assets/werewolf/werewolf_not_animated.json"}
    checks.ensure(required_geometry.issubset(seen), "Dog, helmet, visor, authentic face occluder, and mask asset records exist")
    pinned_files = {file.relative_to(SITE).as_posix() for folder in (SITE / "assets", SITE / "vendor")
                    for file in folder.rglob("*") if file.is_file()}
    checks.ensure(pinned_files == seen, "Every local vendor/model/texture file has exactly one pinned provenance record")
    return manifest


def check_geometry(checks, manifest):
    evidence = []
    for record in manifest["assets"]:
        if record["role"] != "geometry" or record["path"] == "assets/dog/dog_tongue.json":
            continue
        name = record["path"]
        geometry = load_json(local_file(name))
        if name == "assets/werewolf/werewolf_not_animated.json":
            vertices = geometry.get("vertices")
            numeric_array(checks, vertices, name + " legacy vertices")
            checks.ensure(len(vertices) % 3 == 0, name + " vertex shape")
            numeric_array(checks, geometry.get("normals"), name + " normals")
            checks.ensure(isinstance(geometry.get("faces"), list) and len(geometry["faces"]) > 0, name + " encoded faces exist")
            checks.ensure(not geometry.get("morphTargets"), name + " declared static geometry has no morph animation")
            evidence.append({"path": name, "vertices": len(vertices) // 3, "static": True,
                             "triangleIndexValidation": "actual pinned Three JSONLoader in test_wildlife.js"})
            continue
        data = geometry.get("data", {})
        attributes = data.get("attributes", {})
        counts = {}
        expected_attributes = (("position", 3),) if name == "assets/helmet/face.json" else (("position", 3), ("normal", 3), ("uv", 2))
        for attribute, size in expected_attributes:
            item = attributes.get(attribute, {})
            values = item.get("array")
            numeric_array(checks, values, name + " " + attribute)
            checks.ensure(item.get("itemSize") == size and len(values) % size == 0, name + " " + attribute + " shape")
            counts[attribute] = len(values) // size
        checks.ensure(all(count == counts["position"] for count in counts.values()), name + " matching attribute vertex counts")
        indices = data.get("index", {}).get("array")
        checks.ensure(isinstance(indices, list) and len(indices) > 0 and len(indices) % 3 == 0, name + " triangle index data")
        checks.ensure(all(type(index) is int and 0 <= index < counts["position"] for index in indices), name + " index bounds")
        evidence.append({"path": name, "vertices": counts["position"], "triangles": len(indices) // 3})
    tongue = load_json(SITE / "assets" / "dog" / "dog_tongue.json")
    vertices = tongue.get("vertices")
    numeric_array(checks, vertices, "Tongue authored vertices")
    checks.ensure(len(vertices) % 3 == 0, "Tongue vertex shape")
    numeric_array(checks, tongue.get("normals"), "Tongue authored normals")
    checks.ensure(isinstance(tongue.get("uvs"), list) and len(tongue["uvs"]) > 0, "Tongue UV layers")
    for layer in tongue["uvs"]:
        numeric_array(checks, layer, "Tongue UV layer")
        checks.ensure(len(layer) % 2 == 0, "Tongue UV layer shape")
    checks.ensure(isinstance(tongue.get("faces"), list) and len(tongue["faces"]) > 0, "Tongue encoded face data")
    morphs = tongue.get("morphTargets")
    checks.ensure(isinstance(morphs, list) and len(morphs) == 26, "All 26 real tongue animation morph frames")
    names = set()
    different = False
    for target in morphs:
        name = target.get("name")
        checks.ensure(isinstance(name, str) and name.startswith("animation_") and name not in names, "Unique authored morph frame name")
        names.add(name)
        numeric_array(checks, target.get("vertices"), "Morph " + name)
        checks.ensure(len(target["vertices"]) == len(vertices), "Morph frame matches tongue vertex count")
        different = different or target["vertices"] != vertices
    checks.ensure(different, "Animation contains actual deformation data")
    evidence.append({"path": "assets/dog/dog_tongue.json", "vertices": len(vertices) // 3, "morphFrames": len(morphs)})
    return evidence


class PageLinks(HTMLParser):
    def __init__(self):
        super().__init__()
        self.links = []
        self.inline_code = False
        self.inline_style = False
        self.scripts = []

    def handle_starttag(self, tag, attrs):
        attributes = dict(attrs)
        if any(key.startswith("on") for key in attributes):
            self.inline_code = True
        if "style" in attributes or tag == "style":
            self.inline_style = True
        if tag == "script":
            if not attributes.get("src"):
                self.inline_code = True
            else:
                self.scripts.append(attributes["src"])
        if tag in ("script", "img") and attributes.get("src"):
            self.links.append(attributes["src"])
        if tag == "link" and attributes.get("href"):
            self.links.append(attributes["href"])


def check_page_and_scripts(checks):
    index = SITE / "index.html"
    checks.ensure(index.is_file(), "Lab index exists")
    parser = PageLinks()
    parser.feed(index.read_text(encoding="utf-8-sig"))
    checks.ensure(not parser.inline_code, "CSP-compatible external scripts and event listeners")
    checks.ensure(not parser.inline_style, "CSP-compatible external style sheet")
    for link in parser.links:
        if link == "data:,":
            continue  # Empty local favicon; CSP explicitly permits image data URLs.
        checks.ensure(not re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", link) and not link.startswith("//"), "Page resource is local " + link)
        checks.ensure(local_file(link).is_file(), "Page linked file exists " + link)
    checks.ensure(set(RUNTIME_SCRIPTS).issubset(set(parser.scripts)), "All required runtime scripts are linked")
    checks.ensure((SITE / "lab.css").is_file() and (SITE / "NOTICES.txt").is_file(), "Styles and local attribution exist")
    node = shutil.which("node")
    checks.ensure(node is not None, "Existing Node runtime is available")
    for relative in RUNTIME_SCRIPTS:
        path = local_file(relative)
        checks.ensure(path.is_file(), "Runtime script exists " + relative)
        checks.run([node, "--check", str(path)])
    rig_tests = LAB / "tools" / "test_rig.js"
    checks.ensure(rig_tests.is_file(), "Genuine Three geometry/pose rig checks exist")
    checks.run([node, str(rig_tests)])
    checks.run([node, str(LAB / "tools" / "test_lifecycle.js")])
    checks.run([node, str(LAB / "tools" / "test_wildlife.js")])
    checks.run([node, str(LAB / "tools" / "test_original_rigs.js")])
    checks.run([node, str(LAB / "tools" / "test_collection.js")])
    checks.run([node, str(LAB / "tools" / "test_local_model.js")])


def check_http(checks, manifest):
    spec = importlib.util.spec_from_file_location("verified_jeeliz_lab_server", LAB / "server.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    served = []
    server = module.LabServer(port=0)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
    thread.start()
    try:
        checks.ensure(server.server_address[0] == "127.0.0.1", "Actual server socket binds only loopback")
        paths = ["index.html", "assets-manifest.json", "lab.css", "NOTICES.txt"] + [script for script in RUNTIME_SCRIPTS if not script.startswith("vendor/")] + [entry["path"] for entry in manifest["assets"]]
        for relative in paths:
            connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
            connection.request("GET", "/" + relative)
            response = connection.getresponse()
            body = response.read()
            checks.ensure(response.status == 200, "Actual HTTP200 " + relative)
            checks.ensure(body == local_file(relative).read_bytes(), "Actual HTTP bytes match source " + relative)
            checks.ensure("connect-src 'self'" in response.getheader("Content-Security-Policy", ""), "HTTP CSP blocks remote connections " + relative)
            checks.ensure(response.getheader("Permissions-Policy", "").startswith("camera=(self)"), "HTTP camera is local-origin scoped " + relative)
            served.append({"path": relative, "status": response.status, "bytes": len(body), "sha256": hashlib.sha256(body).hexdigest()})
            connection.close()
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
    checks.ensure(not thread.is_alive(), "Verifier ephemeral server stopped")
    checks.run([sys.executable, str(LAB / "tools" / "test_server.py")])
    return served


def source_snapshot():
    paths = [LAB / "server.py", LAB / "Start.cmd", LAB / "Stop.cmd"]
    paths += list((LAB / "tools").glob("*.py")) + list((LAB / "tools").glob("*.js"))
    paths += [file for file in SITE.rglob("*") if file.is_file()]
    return [{"path": str(path.relative_to(LAB)).replace("\\", "/"), "bytes": path.stat().st_size, "sha256": digest(path)}
            for path in sorted(set(paths))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipt", type=Path, help="Optional JSON evidence file; parent directory must already exist")
    args = parser.parse_args()
    checks = Checks()
    receipt = {"schemaVersion": 1, "createdUtc": datetime.now(timezone.utc).isoformat(),
               "labRoot": str(LAB), "upstreamCommit": EXPECTED_COMMIT,
               "cameraRequested": False, "limitations": ["No camera, face tracking, visual quality, or live performance validation in this verifier."]}
    try:
        manifest = check_manifest(checks)
        receipt["pinnedAssetCount"] = len(manifest["assets"])
        receipt["geometry"] = check_geometry(checks, manifest)
        check_page_and_scripts(checks)
        receipt["httpFiles"] = check_http(checks, manifest)
        receipt["sourceFiles"] = source_snapshot()
        receipt["status"] = "PASS"
        exit_code = 0
    except (AssertionError, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        receipt["status"] = "FAIL"
        if str(error) not in checks.failures:
            checks.failures.append(str(error))
        exit_code = 1
    receipt["checkCount"] = checks.count
    receipt["failures"] = checks.failures
    receipt["commands"] = checks.commands
    if args.receipt:
        args.receipt.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": receipt["status"], "checks": checks.count, "pinnedAssets": receipt.get("pinnedAssetCount", 0),
                      "actualHttpFiles": len(receipt.get("httpFiles", [])), "childCommands": len(checks.commands),
                      "cameraRequested": False, "failures": checks.failures}, indent=2))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
