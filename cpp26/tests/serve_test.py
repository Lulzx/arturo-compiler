#!/usr/bin/env python3
"""Serve parity: host interpreter vs compiled binary as HTTP servers.

Starts each engine on its own port with identical route tables, issues
the same raw HTTP requests, and compares status line, headers (except
Date), and body byte-for-byte, plus the normalized startup/log lines.
Invoked from run_tests.sh; exits non-zero on divergence."""
import os
import pathlib
import socket
import subprocess
import sys
import time

CPP = pathlib.Path(__file__).resolve().parent.parent
ARTURO = os.environ.get("ARTURO_HOST", "arturo")
AC = CPP / "bin" / "arturoc"
WORK = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/arturoc-serve")
WORK.mkdir(parents=True, exist_ok=True)
HOST_PORT = 18741
CPP_PORT = 18742
FN_PORT_H = 18743
FN_PORT_C = 18744

ROUTES = r"""
serve .port: PORT [
    GET "/" [ emit "hi" ]
    GET "/user/(?<id>\d+)" $[id][ join ["user " id] ]
    GET "/status" [ emit .status: 404 "missing" ]
    GET "/boom" [ 1 / 0 ]
    GET "/search" $[term][ term ]
    GET "/page" [ send.html "<b>x</b>" ]
    GET "/data" [ send.json "{\"a\":1}" ]
    POST "/echo" $[name][ name ]
]
"""

FUNC = r"""
serve .port: PORT $[req][
    if req\path = "/made" -> return #[status: 201 body: "made"]
    return req\path
]
"""

REQUESTS = [
    ("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", None),
    ("GET /user/42 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", None),
    ("GET /status HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", None),
    ("GET /boom HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", None),
    ("GET /nowhere HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", None),
    ("GET /search?term=hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", None),
    ("GET /page HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", None),
    ("GET /data HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", None),
    ("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: 14\r\nConnection: close\r\n\r\n{\"name\":\"ada\"}", None),
    ("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 12\r\nConnection: close\r\n\r\nname=bob&x=1", None),
]

FUNC_REQUESTS = [
    "GET /abc HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
    "GET /made HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
]


def wait_port(port, proc, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def fetch(port, raw):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        s.settimeout(3)
        s.sendall(raw.encode())
        out = b""
        try:
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                out += chunk
                if b"\r\n\r\n" not in out:
                    continue
                head, _, body = out.partition(b"\r\n\r\n")
                for ln in head.split(b"\r\n"):
                    if ln.lower().startswith(b"content-length:"):
                        if len(body) >= int(ln.split(b":", 1)[1]):
                            return out
        except socket.timeout:
            pass
    return out


def normalize(resp):
    head, _, body = resp.partition(b"\r\n\r\n")
    lines = [ln for ln in head.split(b"\r\n") if not ln.lower().startswith(b"date:")]
    return b"\r\n".join(lines), body


def run_server(cmd, port, requests, label):
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        if not wait_port(port, proc):
            err = proc.stderr.read().decode(errors="replace") if proc.poll() is not None else ""
            raise SystemExit(f"{label}: server did not start (rc={proc.poll()}) err={err!r}")
        return [normalize(fetch(port, r)) for r in requests]
    finally:
        proc.terminate()
        try:
            out, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
        run_server.last_log = out.decode(errors="replace")


def norm_log(log, port):
    out = []
    for line in log.splitlines():
        if "Starting server" in line:
            out.append("START")
        elif line.strip().startswith("--"):
            parts = line.split("|")
            left = parts[0].split("[")[0].strip()
            out.append(left)
    return out


def main():
    routes_src = WORK / "routes.art"
    routes_src.write_text(ROUTES)
    fn_src = WORK / "func.art"
    fn_src.write_text(FUNC)

    def build(src, port, out):
        tmp = WORK / f"tmp{port}.art"
        tmp.write_text(src.read_text().replace("PORT", str(port)))
        b = subprocess.run([str(AC), str(tmp), str(out)], capture_output=True, text=True, timeout=60)
        if b.returncode != 0:
            print(f"arturoc failed for {src.name}: {b.stderr}")
            return None
        return out

    results = {}
    logs = {}
    for label, cmd, port, reqs in [
        ("host", [ARTURO, "--no-color", str(WORK / "tmph.art")], HOST_PORT, [r for r, _ in REQUESTS]),
        ("cpp", None, CPP_PORT, [r for r, _ in REQUESTS]),
    ]:
        if label == "host":
            (WORK / "tmph.art").write_text(ROUTES.replace("PORT", str(HOST_PORT)))
        else:
            binp = build(routes_src, CPP_PORT, WORK / "routesbin")
            if binp is None:
                return 1
            cmd = [str(binp)]
        results[label] = run_server(cmd, port, reqs, label)
        logs[label] = norm_log(run_server.last_log, port)

    if results["host"] != results["cpp"]:
        for i, (h, c) in enumerate(zip(results["host"], results["cpp"])):
            if h != c:
                print(f"DIVERGENCE request {i}:\n host={h!r}\n cpp ={c!r}")
        return 1
    if logs["host"] != logs["cpp"]:
        print(f"LOG DIVERGENCE\n host={logs['host']!r}\n cpp ={logs['cpp']!r}")

    # function form
    fres = {}
    for label, cmd, port in [("host", None, FN_PORT_H), ("cpp", None, FN_PORT_C)]:
        if label == "host":
            p = WORK / "fn_h.art"
            p.write_text(FUNC.replace("PORT", str(FN_PORT_H)))
            cmd = [ARTURO, "--no-color", str(p)]
        else:
            binp = build(fn_src, FN_PORT_C, WORK / "funcbin")
            if binp is None:
                return 1
            cmd = [str(binp)]
        fres[label] = run_server(cmd, port, FUNC_REQUESTS, f"func-{label}")
    if fres["host"] != fres["cpp"]:
        for i, (h, c) in enumerate(zip(fres["host"], fres["cpp"])):
            if h != c:
                print(f"FUNC DIVERGENCE request {i}:\n host={h!r}\n cpp ={c!r}")
        return 1
    print("serve parity: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
