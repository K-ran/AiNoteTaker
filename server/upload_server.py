"""Receives recordings from the board: PUT /upload/<name> -> ./recordings/<name>

Run: python3 server/upload_server.py   (listens on 0.0.0.0:8000)
"""
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "recordings")
MAX_BYTES = 50 * 1024 * 1024


class Handler(BaseHTTPRequestHandler):
    def do_PUT(self):
        name = os.path.basename(self.path)          # strips any ../ tricks
        if not self.path.startswith("/upload/") or not name.upper().endswith(".WAV"):
            return self.send_error(400, "PUT /upload/<name>.wav")
        length = int(self.headers.get("Content-Length", 0))
        if not 0 < length <= MAX_BYTES:
            return self.send_error(413 if length else 411)

        dest = os.path.join(OUT, name)
        if os.path.exists(dest):                     # never overwrite an earlier clip
            stem, ext = os.path.splitext(name)
            i = 1
            while os.path.exists(dest):
                dest = os.path.join(OUT, f"{stem}_{i}{ext}")
                i += 1

        tmp = dest + ".part"
        with open(tmp, "wb") as f:
            remaining = length
            while remaining:
                chunk = self.rfile.read(min(65536, remaining))
                if not chunk:
                    break
                f.write(chunk)
                remaining -= len(chunk)
        if remaining:
            os.remove(tmp)
            return self.send_error(400, "incomplete upload")
        os.rename(tmp, dest)

        print(f"saved {dest} ({length} bytes) from {self.client_address[0]}", flush=True)
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"ok")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    print(f"Saving uploads to {OUT}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", 8000), Handler).serve_forever()
