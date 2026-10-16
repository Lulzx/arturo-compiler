#!/usr/bin/env python3
"""HTTP parity: host interpreter vs compiled binary against a local
HTTP server (status/version/body, header type conversions, GET query,
POST forms, failed requests, download). Invoked from run_tests.sh;
exits non-zero on divergence in stdout or exit status."""
import http.server
import os
import pathlib
import subprocess
import sys
import threading

CPP = pathlib.Path(__file__).resolve().parent.parent
ARTURO = os.environ.get("ARTURO_HOST", "arturo")
AC = CPP / "bin" / "arturoc"
WORK = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/arturoc-http")
WORK.mkdir(parents=True, exist_ok=True)
PORT = 18411

PROG = """
r: request "http://127.0.0.1:PORT/get" #[some: "arg" another: 123]
print r\\status
print r\\version
print r\\body
print get r\\headers "content-length"
print type get r\\headers "content-length"
print get r\\headers "x-multi"
print get r\\headers "access-control-allow-credentials"
print type get r\\headers "date"
p: request.post "http://127.0.0.1:PORT/post" #[a: "1"]
print p\\status
j: request.post.json "http://127.0.0.1:PORT/j" #[a: "1"]
print j\\status
w: request.raw "http://127.0.0.1:PORT/raw" #[]
print w\\status
print request "http://127.0.0.1:19998/none" #[]
download "http://127.0.0.1:PORT/dl/file.txt"
print read "file.txt"
"""


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _reply(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)
        data = b'{"ok":true}'
        if self.path.startswith("/dl/"):
            data = b"FILEDATA-123"
        self.send_response(201)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("X-Multi", "one")
        self.send_header("X-Multi", "two")
        self.send_header("Access-Control-Allow-Credentials", "true")
        self.end_headers()
        self.wfile.write(data)

    do_GET = _reply
    do_POST = _reply
    do_PUT = _reply
    do_DELETE = _reply
    do_PATCH = _reply


def main():
    server = http.server.HTTPServer(("127.0.0.1", PORT), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    src = WORK / "httpprog.art"
    src.write_text(PROG.replace("PORT", str(PORT)))
    host_dir = WORK / "host"
    cpp_dir = WORK / "cpp"
    host_dir.mkdir(exist_ok=True)
    cpp_dir.mkdir(exist_ok=True)
    host = subprocess.run([ARTURO, "--no-color", str(src)], cwd=host_dir,
                          capture_output=True, text=True, timeout=30)
    binary = WORK / "httpbin"
    build = subprocess.run([str(AC), str(src), str(binary)], capture_output=True, text=True, timeout=30)
    if build.returncode != 0:
        print(f"arturoc failed: {build.stderr}")
        return 1
    cpp = subprocess.run([str(binary)], cwd=cpp_dir, capture_output=True, text=True, timeout=30)
    server.shutdown()
    if (host.stdout, host.returncode) != (cpp.stdout, cpp.returncode):
        print(f"DIVERGENCE\n host=({host.returncode}) {host.stdout!r}\n cpp =({cpp.returncode}) {cpp.stdout!r}\n cpp-err={cpp.stderr!r}")
        return 1
    if cpp.stderr:
        print(f"compiled binary wrote to stderr: {cpp.stderr!r}")
        return 1
    print("http parity: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
