#!/usr/bin/env python3
"""Actual loopback HTTP and project-control tests; never request a camera."""
from __future__ import annotations

import hashlib
import http.client
import importlib.util
import json
import os
import shutil
import subprocess
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest

sys.dont_write_bytecode = True
LAB = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("jeeliz_lab_server", LAB / "server.py")
server_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server_module)


class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="prismforge-jeeliz-http-")
        cls.root = Path(cls.temporary.name) / "site"
        cls.root.mkdir()
        cls.html = b"<!doctype html><title>Local Jeeliz test fixture</title>"
        (cls.root / "index.html").write_bytes(cls.html)
        (cls.root / "lab.js").write_bytes(b"'use strict';\n")
        (cls.root / "data.json").write_text('{"fixture":true}', encoding="utf-8")
        (cls.root / "private-dir").mkdir()
        (cls.root.parent / "outside.txt").write_text("outside-root-sentinel", encoding="utf-8")
        cls.server = server_module.LabServer(cls.root, port=0, token="test-token-" + "a" * 40)
        cls.thread = threading.Thread(target=cls.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)
        cls.temporary.cleanup()

    def request(self, path, method="GET", headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=2)
        connection.request(method, path, headers=headers or {})
        response = connection.getresponse()
        result = response.status, dict(response.getheaders()), response.read()
        connection.close()
        return result

    def test_01_loopback_binding(self):
        self.assertEqual(self.server.server_address[0], "127.0.0.1")
        self.assertGreater(self.server.server_port, 0)

    def test_02_site_root_bytes(self):
        status, headers, body = self.request("/")
        self.assertEqual(status, 200)
        self.assertEqual(body, self.html)
        self.assertEqual(int(headers["Content-Length"]), len(self.html))
        self.assertIn("text/html", headers["Content-Type"])
        self.assertEqual(self.request("/index.html?cache-test=1")[2], self.html)

    def test_03_head_no_body(self):
        status, headers, body = self.request("/", "HEAD")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"")
        self.assertEqual(int(headers["Content-Length"]), len(self.html))

    def test_04_external_network_and_permissions_headers(self):
        status, headers, _ = self.request("/")
        self.assertEqual(status, 200)
        csp = headers["Content-Security-Policy"]
        self.assertIn("connect-src 'self'", csp)
        self.assertIn("script-src 'self'", csp)
        self.assertIn("img-src 'self' data: blob:", csp)
        self.assertIn("frame-ancestors 'none'", csp)
        self.assertNotIn("unsafe-inline", csp)
        self.assertNotIn("https:", csp)
        self.assertEqual(headers["Permissions-Policy"], "camera=(self), microphone=(), geolocation=()")
        self.assertEqual(headers["X-Content-Type-Options"], "nosniff")
        self.assertEqual(headers["Cache-Control"], "no-store")

    def test_05_script_json_mime(self):
        self.assertIn("text/javascript", self.request("/lab.js")[1]["Content-Type"])
        self.assertIn("application/json", self.request("/data.json")[1]["Content-Type"])

    def test_06_missing_files_and_no_listing(self):
        for path in ("/missing.html", "/private-dir", "/private-dir/", "/server.py", "/.runtime/server-state.json"):
            with self.subTest(path=path):
                status, headers, body = self.request(path)
                self.assertEqual(status, 404)
                self.assertNotIn(b"outside-root-sentinel", body)
                self.assertIn("Content-Security-Policy", headers)

    def test_07_traversal_encoding_and_windows_paths(self):
        cases = ("/../outside.txt", "/%2e%2e/outside.txt", "/%2E%2E%2Foutside.txt", "/%252e%252e/outside.txt",
                 "/.%2e/outside.txt", "/a/../../outside.txt", "/a/./lab.js", "/a%5c..%5coutside.txt",
                 "/C:%5cWindows%5cwin.ini", "/index.html:ads", "/%00", "/%1f", "/%7f", "/%ff",
                 "/%", "/%GG", "//outside.txt", "/%2foutside.txt")
        for path in cases:
            with self.subTest(path=path):
                status, _, body = self.request(path)
                self.assertEqual(status, 400)
                self.assertNotIn(b"outside-root-sentinel", body)

    def test_08_wrong_host_rejected(self):
        self.assertEqual(self.request("/", headers={"Host": "example.com"})[0], 403)
        self.assertEqual(self.request("/", headers={"Host": "127.0.0.1:1"})[0], 403)

    def test_09_methods_are_read_only(self):
        for method in ("POST", "PUT", "DELETE", "PATCH"):
            with self.subTest(method=method):
                self.assertEqual(self.request("/index.html", method)[0], 405)
        self.assertEqual(self.request("/__lab__/stop")[0], 404)

    def test_10_control_token_and_origin(self):
        self.assertEqual(self.request("/__lab__/status")[0], 403)
        self.assertEqual(self.request("/__lab__/stop", "POST")[0], 403)
        self.assertEqual(self.request("/__lab__/stop", "POST", {"X-Jeeliz-Lab-Token": "wrong"})[0], 403)
        self.assertEqual(self.request("/__lab__/stop", "POST", {"X-Jeeliz-Lab-Token": "caf\u00e9"})[0], 403)
        headers = {"X-Jeeliz-Lab-Token": self.server.token, "Origin": "https://example.com"}
        self.assertEqual(self.request("/__lab__/status", headers=headers)[0], 403)
        self.assertEqual(self.request("/__lab__/stop", "POST", headers)[0], 403)

    def test_11_scoped_server_status(self):
        status, _, body = self.request("/__lab__/status", headers={"X-Jeeliz-Lab-Token": self.server.token})
        self.assertEqual(status, 200)
        state = json.loads(body)
        self.assertEqual(state["project"], server_module.PROJECT)
        self.assertEqual(state["port"], self.server.server_port)
        self.assertNotIn("token", state)
        self.assertNotIn(self.server.token.encode(), body)

    def test_12_resolved_containment(self):
        self.assertEqual(server_module.safe_site_path(self.root, "/"), (self.root / "index.html").resolve())
        self.assertEqual(server_module.safe_site_path(self.root, "/data%2ejson"), (self.root / "data.json").resolve())
        for path in ("https://example.com/", "/../outside.txt", "/a%5cb", "/index.html:x", "/#frag"):
            with self.subTest(path=path):
                with self.assertRaises(ValueError):
                    server_module.safe_site_path(self.root, path)

    def test_13_control_identity_validation(self):
        state = {"project": self.server.project, "pid": 123, "port": self.server.server_port, "token": self.server.token}
        with self.assertRaises(ValueError):
            server_module.control(state)

    def test_14_control_redirects_cannot_leak_token(self):
        received = []

        class Sink(server_module.BaseHTTPRequestHandler):
            def do_GET(self):
                received.append(self.headers.get("X-Jeeliz-Lab-Token"))
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *args):
                pass

        sink = server_module.ThreadingHTTPServer(("127.0.0.1", 0), Sink)

        class Redirector(server_module.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(302)
                self.send_header("Location", f"http://127.0.0.1:{sink.server_port}/other-origin")
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *args):
                pass

        redirector = server_module.ThreadingHTTPServer(("127.0.0.1", 0), Redirector)
        threads = [threading.Thread(target=instance.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
                   for instance in (sink, redirector)]
        for thread in threads:
            thread.start()
        try:
            state = {"pid": 1, "port": redirector.server_port, "token": self.server.token}
            with self.assertRaises(server_module.HTTPError) as caught:
                server_module.control(state)
            self.assertEqual(caught.exception.code, 302)
            self.assertEqual(received, [], "A redirect cannot forward the project control token to another origin")
            handler = server_module.NoControlRedirect()
            self.assertIsNone(handler.redirect_request(None, None, 302, "Found", {}, "https://example.invalid/"))
        finally:
            for instance in (sink, redirector):
                instance.shutdown()
                instance.server_close()
            for thread in threads:
                thread.join(timeout=2)

    def test_99_authenticated_stop(self):
        status, _, body = self.request("/__lab__/stop", "POST", {"X-Jeeliz-Lab-Token": self.server.token})
        self.assertEqual(status, 204)
        self.assertEqual(body, b"")
        self.thread.join(timeout=2)
        self.assertFalse(self.thread.is_alive())


class LifecycleTests(unittest.TestCase):
    def test_cli_lifecycle_spaces_proxy_and_scope(self):
        with tempfile.TemporaryDirectory(prefix="PrismForge Jeeliz lab with spaces ") as directory:
            root = Path(directory)
            script = root / "server.py"
            shutil.copyfile(LAB / "server.py", script)
            (root / "site").mkdir()
            (root / "site" / "index.html").write_text("Local lifecycle fixture", encoding="utf-8")
            state_path = root / ".runtime" / "server-state.json"
            environment = os.environ.copy()
            environment.update({"http_proxy": "http://127.0.0.1:1", "HTTP_PROXY": "http://127.0.0.1:1",
                                "https_proxy": "http://127.0.0.1:1", "HTTPS_PROXY": "http://127.0.0.1:1",
                                "no_proxy": "", "NO_PROXY": "", "PYTHONDONTWRITEBYTECODE": "1"})
            child = subprocess.Popen([sys.executable, str(script), "start", "--port", "0"],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=environment)
            saved_state = None
            try:
                deadline = time.monotonic() + 5
                while not state_path.exists() and child.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertTrue(state_path.is_file(), "Owned copied server wrote its project state")
                saved_state = state_path.read_bytes()
                state = json.loads(saved_state)
                self.assertEqual(state["pid"], child.pid)
                self.assertIn("prismforge-jeeliz-3d-lab:", state["project"])

                def command(*arguments):
                    return subprocess.run([sys.executable, str(script), *arguments], capture_output=True,
                                          text=True, timeout=5, env=environment)

                # Poisoned environment proxies must never receive the loopback control token.
                status = command("status")
                self.assertEqual(status.returncode, 0, status.stderr)
                duplicate = command("start", "--port", "0")
                self.assertEqual(duplicate.returncode, 0, duplicate.stderr)
                self.assertEqual(state_path.read_bytes(), saved_state, "Duplicate Start preserves the actual owner PID/token")

                wrong_project = dict(state, project="another-project")
                state_path.write_text(json.dumps(wrong_project), encoding="utf-8")
                denied = command("stop")
                self.assertEqual(denied.returncode, 1)
                self.assertIsNone(child.poll(), "Wrong project state cannot stop the owned server")
                state_path.write_bytes(saved_state)

                stopped = command("stop")
                self.assertEqual(stopped.returncode, 0, stopped.stderr)
                stdout, stderr = child.communicate(timeout=5)
                self.assertEqual(child.returncode, 0, stderr)
                self.assertFalse(state_path.exists(), "Owned runtime state removed after authenticated stop")
                self.assertEqual(command("stop").returncode, 0, "A second Stop is harmless")
            finally:
                if child.poll() is None:
                    if saved_state is not None:
                        state_path.write_bytes(saved_state)
                        subprocess.run([sys.executable, str(script), "stop"], capture_output=True, timeout=5, env=environment)
                    try:
                        child.communicate(timeout=5)
                    except subprocess.TimeoutExpired:
                        # This handle belongs solely to the freshly copied test child.
                        child.terminate()
                        child.communicate(timeout=5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
