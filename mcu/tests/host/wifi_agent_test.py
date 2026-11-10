"""Exercise the actual CLI client over HTTP, including streaming and no-overwrite."""
import contextlib
import io
import importlib.util
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import tempfile
import threading
import unittest
from urllib.parse import parse_qs, urlsplit
import zlib

MODULE = Path(__file__).resolve().parents[2] / "tools/chromagic_wifi.py"
SPEC = importlib.util.spec_from_file_location("chromagic_wifi", MODULE)
CLIENT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CLIENT)


class Server(BaseHTTPRequestHandler):
    files = {}
    corrupt = False
    def log_message(self, *_): pass
    def reply(self, code, value):
        data = json.dumps(value).encode()
        self.send_response(code); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
    def authorized(self):
        if self.headers.get("X-ChroMagic-Token") == "test-token": return True
        self.reply(401, {"error": "Pair first"}); return False
    def do_POST(self):
        if self.path == "/api/session":
            data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            self.reply(200 if data["code"] == "012345" else 401, {"token": "test-token"})
        elif self.authorized(): self.reply(202 if self.path == "/api/networks" else 201, {"ok": True})
    def do_PUT(self):
        if not self.authorized(): return
        path = parse_qs(urlsplit(self.path).query)["path"][0]
        data = self.rfile.read(int(self.headers["Content-Length"]))
        if path in self.files: self.reply(409, {"error": "Exists"}); return
        self.files[path] = data; self.reply(201, {"ok": True})
    def do_GET(self):
        if not self.authorized(): return
        route = urlsplit(self.path)
        if route.path == "/api/status": self.reply(200, {"api_version": 1}); return
        if route.path == "/api/networks": self.reply(200, {"scanning": False, "error": "", "networks": [{"ssid": "Home", "rssi": -40, "secure": True, "supported": True}]}); return
        path = parse_qs(route.query)["path"][0]
        data = self.files[path]
        if route.path == "/api/checksum": self.reply(200, {"bytes": len(data), "crc32": f"{zlib.crc32(data):08x}"}); return
        if self.corrupt: data = b"bad"
        self.send_response(200); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)


class Tests(unittest.TestCase):
    def test_roundtrip_and_failures(self):
        server = ThreadingHTTPServer(("127.0.0.1", 0), Server)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        try:
            client = CLIENT.Client(f"http://127.0.0.1:{server.server_port}")
            self.assertEqual(client.pair("012345"), "test-token")
            self.assertEqual(client.request("GET", "status")["api_version"], 1)
            output=io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(CLIENT.main(["--url", f"http://127.0.0.1:{server.server_port}", "--token", "test-token", "networks"]), 0)
            self.assertEqual(json.loads(output.getvalue())["networks"][0]["ssid"], "Home")
            with tempfile.TemporaryDirectory() as folder:
                folder = Path(folder); source = folder / "input.bin"; source.write_bytes(bytes(range(256)) * 1025)
                remote = "/CHROMAGIC/A & B.bin"
                client.put(source, remote)
                destination = folder / "output.bin"
                client.get(remote, destination)
                self.assertEqual(destination.read_bytes(), source.read_bytes())
                with self.assertRaises(FileExistsError): client.get(remote, destination)
                with self.assertRaises(RuntimeError): client.put(source, remote)
                Server.corrupt = True
                with self.assertRaises(RuntimeError): client.get(remote, folder / "bad.bin")
                self.assertFalse((folder / "bad.bin").exists())
                self.assertFalse(list(folder.glob("*.part")))
                Server.corrupt = False
        finally:
            server.shutdown(); server.server_close(); thread.join()
    def test_local_address_validation(self):
        for url in ["https://192.168.1.2", "http://example.com", "http://8.8.8.8", "http://user:pass@192.168.1.2", "http://192.168.1.2/path"]:
            with self.assertRaises(ValueError): CLIENT.Client(url)


if __name__ == "__main__": unittest.main()
