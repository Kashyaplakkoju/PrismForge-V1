#!/usr/bin/env python3
"""Loopback-only, project-scoped server for the isolated Jeeliz experiment."""
from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import logging
import mimetypes
import os
from pathlib import Path
import re
import secrets
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError, URLError
from urllib.parse import unquote, urlsplit
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener
import webbrowser

ROOT = Path(__file__).resolve().parent
SITE = ROOT / "site"
RUNTIME = ROOT / ".runtime"
STATE = RUNTIME / "server-state.json"
PROJECT = "prismforge-jeeliz-3d-lab:" + hashlib.sha256(str(ROOT).encode("utf-8")).hexdigest()
DEFAULT_PORT = 8743
class NoControlRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, new_url):
        return None


CONTROL_HTTP = build_opener(ProxyHandler({}), NoControlRedirect())
CSP = (
    "default-src 'self'; script-src 'self'; style-src 'self'; "
    "img-src 'self' data: blob:; connect-src 'self'; "
    "worker-src 'self' blob:; media-src 'self' blob:; "
    "object-src 'none'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'"
)


def safe_site_path(root: Path, target: str) -> Path:
    """Validate before normalization; reject both one and double encoded traversal."""
    parsed = urlsplit(target)
    if parsed.scheme or parsed.netloc or parsed.fragment:
        raise ValueError("Only a local origin-form path is accepted")
    raw = parsed.path
    if not raw.startswith("/") or raw.startswith("//"):
        raise ValueError("Invalid path")
    if re.search(r"%(?![0-9a-fA-F]{2})", raw):
        raise ValueError("Malformed percent escape")
    decoded = unquote(raw, encoding="utf-8", errors="strict")
    if re.search(r"%[0-9a-fA-F]{2}", decoded):
        raise ValueError("Nested percent encoding is forbidden")
    if any(ord(char) < 32 or ord(char) == 127 for char in decoded):
        raise ValueError("Control characters are forbidden")
    if "\\" in decoded or ":" in decoded or decoded.startswith("//"):
        raise ValueError("Non-site path syntax is forbidden")
    parts = decoded[1:].split("/")
    if any(part in (".", "..") for part in parts):
        raise ValueError("Traversal is forbidden")
    if decoded == "/":
        parts = ["index.html"]
    candidate = root.joinpath(*parts).resolve()
    if not candidate.is_relative_to(root.resolve()):
        raise ValueError("Path escapes site root")
    return candidate


class LabServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, site_root: Path = SITE, port: int = DEFAULT_PORT, token: str | None = None):
        self.site_root = site_root.resolve()
        self.token = token or secrets.token_urlsafe(32)
        self.project = PROJECT
        super().__init__(("127.0.0.1", port), LabHandler)
        self.origin = "http://127.0.0.1:" + str(self.server_port)


class LabHandler(BaseHTTPRequestHandler):
    server: LabServer
    server_version = "PrismForgeJeelizLab/1"
    sys_version = ""

    def end_headers(self):
        self.send_header("Content-Security-Policy", CSP)
        self.send_header("Permissions-Policy", "camera=(self), microphone=(), geolocation=()")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        logging.info("%s %s", self.client_address[0], fmt % args)

    def local_host(self):
        if self.client_address[0] != "127.0.0.1" or self.headers.get("Host") != f"127.0.0.1:{self.server.server_port}":
            self.send_error(403, "Only this loopback origin is accepted")
            return False
        return True

    def authorized(self):
        if not self.local_host():
            return False
        supplied = self.headers.get("X-Jeeliz-Lab-Token", "")
        origin = self.headers.get("Origin")
        if (origin is not None and origin != self.server.origin) or not hmac.compare_digest(supplied.encode("utf-8"), self.server.token.encode("utf-8")):
            self.send_error(403, "Project control token required")
            return False
        return True

    def do_GET(self):
        self.serve_static(head=False)

    def do_HEAD(self):
        self.serve_static(head=True)

    def serve_static(self, head: bool):
        if not self.local_host():
            return
        if self.path == "/__lab__/status":
            if not self.authorized():
                return
            data = json.dumps({"project": self.server.project, "pid": os.getpid(), "port": self.server.server_port}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            if not head:
                self.wfile.write(data)
            return
        try:
            path = safe_site_path(self.server.site_root, self.raw_requestline.split()[1].decode("iso-8859-1"))
        except (ValueError, UnicodeError, OSError):
            self.send_error(400, "Unsafe path")
            return
        if not path.is_file():
            self.send_error(404, "Site file not found")
            return
        try:
            body = path.read_bytes()
        except OSError:
            self.send_error(404, "Site file not available")
            return
        content_type = mimetypes.guess_type(str(path))[0] or "application/octet-stream"
        if path.suffix.lower() in (".js", ".mjs"):
            content_type = "text/javascript"
        if path.suffix.lower() in (".html", ".css", ".js", ".mjs", ".json", ".txt"):
            content_type += "; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if not head:
            self.wfile.write(body)

    def do_POST(self):
        if self.path != "/__lab__/stop":
            self.send_error(405, "Only the project stop endpoint accepts POST")
            return
        if not self.authorized():
            return
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()
        threading.Thread(target=self.server.shutdown, daemon=True).start()

    def do_PUT(self):
        self.send_error(405, "Read-only site")

    do_DELETE = do_PUT
    do_PATCH = do_PUT


def read_state():
    if RUNTIME.is_symlink() or RUNTIME.resolve().parent != ROOT:
        raise ValueError("Runtime directory must remain inside this lab")
    if not STATE.exists():
        return None
    if STATE.is_symlink() or STATE.resolve().parent != RUNTIME.resolve():
        raise ValueError("Unsafe state file")
    data = json.loads(STATE.read_text(encoding="utf-8"))
    if data.get("project") != PROJECT or type(data.get("pid")) is not int or data["pid"] <= 0:
        raise ValueError("State does not belong to this project")
    if type(data.get("port")) is not int or not 1 <= data["port"] <= 65535:
        raise ValueError("Invalid project port")
    if not isinstance(data.get("token"), str) or re.fullmatch(r"[A-Za-z0-9_-]{32,128}", data["token"]) is None:
        raise ValueError("Invalid project control token")
    return data


def control(state, action="status"):
    request = Request(f"http://127.0.0.1:{state['port']}/__lab__/{action}",
                      headers={"X-Jeeliz-Lab-Token": state["token"]},
                      method="POST" if action == "stop" else "GET")
    with CONTROL_HTTP.open(request, timeout=2) as response:
        if action == "status":
            current = json.load(response)
            if current != {"project": PROJECT, "pid": state["pid"], "port": state["port"]}:
                raise ValueError("Running server identity differs from this project state")
            return current
        return response.status == 204


def cleanup_owned_state(token):
    try:
        state = read_state()
        if state is not None and hmac.compare_digest(state["token"], token):
            STATE.unlink()
    except (OSError, ValueError, json.JSONDecodeError):
        logging.exception("State cleanup failed")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", nargs="?", choices=("start", "serve", "stop", "status"), default="start")
    parser.add_argument("--port", type=int, default=int(os.environ.get("PRISMFORGE_JEELIZ_PORT", DEFAULT_PORT)))
    parser.add_argument("--open-browser", action="store_true")
    args = parser.parse_args(argv)
    if not 0 <= args.port <= 65535:
        parser.error("Port must be 0-65535")
    try:
        state = read_state()
        if args.action in ("stop", "status"):
            if state is None:
                print("Jeeliz lab is not running.")
                return 0 if args.action == "stop" else 1
            control(state)
            if args.action == "stop":
                control(state, "stop")
                print("Stopped this Jeeliz lab server.")
            else:
                print(f"Jeeliz lab: http://127.0.0.1:{state['port']}/")
            return 0
        if state is not None:
            try:
                control(state)
                if args.open_browser:
                    webbrowser.open(f"http://127.0.0.1:{state['port']}/")
                print("This Jeeliz lab server is already running.")
                return 0
            except (OSError, ValueError, HTTPError, URLError):
                # A stale record never authorizes killing a process or stopping a different server.
                pass
        if RUNTIME.is_symlink() or RUNTIME.resolve().parent != ROOT:
            raise ValueError("Runtime directory must remain inside this lab")
        RUNTIME.mkdir(exist_ok=True)
        logging.basicConfig(filename=str(RUNTIME / "server.log"), level=logging.INFO,
                            format="%(asctime)s %(levelname)s %(message)s")
        with LabServer(port=args.port) as server:
            live_state = {"project": PROJECT, "pid": os.getpid(), "port": server.server_port, "token": server.token}
            temporary = RUNTIME / f"server-state-{os.getpid()}.tmp"
            temporary.write_text(json.dumps(live_state, indent=2), encoding="utf-8")
            os.replace(temporary, STATE)
            logging.info("Serving site at %s", server.origin)
            if args.open_browser:
                webbrowser.open(server.origin + "/")
            try:
                server.serve_forever(poll_interval=0.2)
            finally:
                cleanup_owned_state(server.token)
        return 0
    except (OSError, ValueError, HTTPError, URLError, json.JSONDecodeError) as error:
        logging.error("Server action failed: %s", error)
        print("Jeeliz lab server action failed: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
