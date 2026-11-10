#!/usr/bin/env python3
"""Local ChroMagic Wi-Fi API client. Standard library only; no cloud service."""
import argparse
import getpass
import http.client
import ipaddress
import time
import json
import os
from pathlib import Path
import sys
import tempfile
from urllib.parse import quote, urlsplit
import zlib


class Client:
    def __init__(self, url, token="", timeout=60):
        parsed = urlsplit(url)
        if parsed.scheme != "http" or not parsed.hostname or parsed.username or parsed.password or parsed.path not in ("", "/") or parsed.query or parsed.fragment:
            raise ValueError("Use the local http://IP address shown on the Chromatic.")
        address = ipaddress.ip_address(parsed.hostname)
        if address.version != 4 or not address.is_private:
            raise ValueError("This client is intended for a local IPv4 address.")
        self.host, self.port, self.token, self.timeout = parsed.hostname, parsed.port or 80, token, timeout

    def _open(self, method, route, path=None, body=None, extra_headers=None):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
        target = "/api/" + route + ("?path=" + quote(path, safe="") if path is not None else "")
        headers = {"X-ChroMagic-Token": self.token}
        headers.update(extra_headers or {})
        try:
            connection.request(method, target, body=body, headers=headers)
            response = connection.getresponse()
            if response.status >= 400:
                data = response.read(8192).decode("utf-8", errors="replace")
                try:
                    data = json.loads(data).get("error", data)
                except (ValueError, AttributeError):
                    pass
                raise RuntimeError(f"HTTP {response.status}: {data}")
            return connection, response
        except BaseException:
            connection.close()
            raise

    def request(self, method, route, path=None, value=None):
        body = json.dumps(value).encode() if value is not None else None
        headers = {"Content-Type": "application/json"} if value is not None else {}
        connection, response = self._open(method, route, path, body, headers)
        try:
            data = response.read().decode("utf-8")
            if route == "list":
                return [json.loads(line) for line in data.splitlines() if line]
            return json.loads(data)
        finally:
            connection.close()

    def pair(self, code):
        self.token = self.request("POST", "session", value={"code": code.strip().lower()})["token"]
        return self.token

    def put(self, source, destination):
        source = Path(source)
        if not source.is_file():
            raise ValueError("Upload source must be a regular file.")
        length = source.stat().st_size
        if length > 128 * 1024 * 1024:
            raise ValueError("Maximum upload size is 128 MiB.")
        # A bounded reader streams the file; HTTPConnection does not load it all.
        class Reader:
            def __init__(self, stream):
                self.stream, self.remaining, self.crc = stream, length, 0
            def read(self, amount):
                if not self.remaining:
                    return b""
                data = self.stream.read(min(amount, self.remaining))
                if not data:
                    raise RuntimeError("Source file was shortened during upload.")
                self.remaining -= len(data)
                self.crc = zlib.crc32(data, self.crc)
                return data
        with source.open("rb") as stream:
            reader = Reader(stream)
            connection, response = self._open("PUT", "file", destination, reader, {"Content-Length": str(length), "Content-Type": "application/octet-stream"})
            try:
                response.read()
                if response.status != 201 or reader.remaining:
                    raise RuntimeError("Upload was not acknowledged as complete.")
            finally:
                connection.close()
        remote = self.request("GET", "checksum", destination)
        if remote["bytes"] != length or remote["crc32"] != f"{reader.crc:08x}":
            raise RuntimeError("Uploaded file failed verification; inspect it before use.")
        return {"ok": True, "path": destination, **remote}

    def get(self, source, destination):
        destination = Path(destination)
        if destination.exists():
            raise FileExistsError("Destination exists; it will not be overwritten.")
        expected = self.request("GET", "checksum", source)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(prefix=".chromagic-", suffix=".part", dir=destination.parent, delete=False) as stream:
                temporary = Path(stream.name)
                connection, response = self._open("GET", "file", source)
                crc, length = 0, 0
                try:
                    while True:
                        data = response.read(65536)
                        if not data:
                            break
                        stream.write(data)
                        crc, length = zlib.crc32(data, crc), length + len(data)
                finally:
                    connection.close()
                stream.flush()
                os.fsync(stream.fileno())
            if length != expected["bytes"] or f"{crc:08x}" != expected["crc32"]:
                raise RuntimeError("Download incomplete or checksum mismatch; no destination file published.")
            # Atomic no-overwrite publication, including a competing local writer.
            os.link(temporary, destination)
            return {"ok": True, "path": str(destination), **expected}
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default=os.environ.get("CHROMAGIC_URL"), help="http://IP shown on the Chromatic, or CHROMAGIC_URL")
    parser.add_argument("--token", default=os.environ.get("CHROMAGIC_TOKEN", ""), help="Session token; prefer CHROMAGIC_TOKEN to shell history")
    parser.add_argument("--timeout", type=int, default=60)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("pair", help="Prompt for the device code and print a session token")
    sub.add_parser("status")
    sub.add_parser("networks", help="Scan nearby 2.4 GHz Wi-Fi networks")
    listing = sub.add_parser("ls"); listing.add_argument("path", nargs="?", default="/")
    for name in ("mkdir", "checksum"):
        command = sub.add_parser(name); command.add_argument("path")
    for name in ("put", "get"):
        command = sub.add_parser(name); command.add_argument("source"); command.add_argument("destination")
    args = parser.parse_args(argv)
    if not args.url:
        parser.error("--url or CHROMAGIC_URL is required")
    try:
        client = Client(args.url, args.token, args.timeout)
        if args.command == "pair" or not client.token:
            code = os.environ.get("CHROMAGIC_CODE") or getpass.getpass("Web code shown on Chromatic: ")
            client.pair(code)
        if args.command == "pair":
            print(client.token)
            return 0
        if args.command == "status": result = client.request("GET", "status")
        elif args.command == "networks":
            client.request("POST", "networks")
            for _ in range(60):
                time.sleep(0.3)
                result = client.request("GET", "networks")
                if not result["scanning"]:
                    if result.get("error"): raise RuntimeError(result["error"])
                    break
            else: raise RuntimeError("Scan timed out; try again shortly.")
        elif args.command == "ls": result = client.request("GET", "list", args.path)
        elif args.command == "mkdir": result = client.request("POST", "folder", args.path)
        elif args.command == "checksum": result = client.request("GET", "checksum", args.path)
        elif args.command == "put": result = client.put(args.source, args.destination)
        else: result = client.get(args.source, args.destination)
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, RuntimeError, http.client.HTTPException) as error:
        print(json.dumps({"error": str(error)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
