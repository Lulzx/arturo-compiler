#!/usr/bin/env python3
"""Live socket parity: host interpreter vs compiled binary, client and
server scenarios over real TCP on localhost. Invoked from run_tests.sh;
exits non-zero on any divergence in stdout, exit status, or wire bytes."""
import os
import pathlib
import socket
import subprocess
import sys
import threading
import time

CPP = pathlib.Path(__file__).resolve().parent.parent
ARTURO = os.environ.get("ARTURO_HOST", "arturo")
AC = CPP / "bin" / "arturoc"
WORK = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/arturoc-sock")
WORK.mkdir(parents=True, exist_ok=True)
PORT = 18311

CLIENT = """
s: connect PORT
print type s
print s
print socket? s
send s "hello"
print send? s "raw"
print receive s
send.chunk s "two-lines"
print receive s
print receive s
unplug s
print socket? 42
"""

SERVER = """
srv: listen PORT
print type srv
cli: accept srv
print receive cli
send cli "pong"
unplug cli
unplug srv
print "served"
"""


def echo_server(results):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(5)
    conn, _ = srv.accept()
    conn.settimeout(5)
    buf = b""
    try:
        while b"\n" not in buf:
            data = conn.recv(4096)
            if not data:
                break
            buf += data
    except OSError:
        pass
    results.append(buf)
    conn.sendall(b"echo:" + buf.split(b"\r\n")[0] + b"\r\n")
    conn.sendall(b"line-one\r\nline-two\r\n")
    time.sleep(0.3)
    conn.close()
    srv.close()


def compile_and_run(src: pathlib.Path, binary: pathlib.Path):
    r = subprocess.run([str(AC), str(src), str(binary)], capture_output=True, text=True, timeout=20)
    if r.returncode != 0:
        raise RuntimeError(f"arturoc failed: {r.stderr}")
    r = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
    return r.stdout, r.returncode


def run_host(src: pathlib.Path):
    r = subprocess.run([ARTURO, "--no-color", str(src)], capture_output=True, text=True, timeout=20)
    return r.stdout, r.returncode


def client_scenario(runner):
    results = []
    thread = threading.Thread(target=echo_server, args=(results,))
    thread.start()
    src = WORK / "sockc.art"
    src.write_text(CLIENT.replace("PORT", str(PORT)))
    out, rc = run_host(src) if runner == "host" else compile_and_run(src, WORK / "sockc")
    thread.join(timeout=10)
    return out, rc, results


def server_scenario(runner):
    src = WORK / "socks.art"
    src.write_text(SERVER.replace("PORT", str(PORT)))
    if runner == "host":
        proc = subprocess.Popen([ARTURO, "--no-color", str(src)], stdout=subprocess.PIPE, text=True)
    else:
        binary = WORK / "socks"
        r = subprocess.run([str(AC), str(src), str(binary)], capture_output=True, text=True, timeout=20)
        if r.returncode != 0:
            raise RuntimeError(f"arturoc failed: {r.stderr}")
        proc = subprocess.Popen([str(binary)], stdout=subprocess.PIPE, text=True)
    time.sleep(1.0)
    client = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    client.sendall(b"ping\r\n")
    got = client.recv(4096)
    client.close()
    out, _ = proc.communicate(timeout=15)
    return out, got


def main():
    failures = 0
    host = client_scenario("host")
    cpp = client_scenario("cpp")
    if host != cpp:
        failures += 1
        print(f"CLIENT DIVERGENCE\n host={host}\n cpp ={cpp}")
    host = server_scenario("host")
    cpp = server_scenario("cpp")
    if host != cpp:
        failures += 1
        print(f"SERVER DIVERGENCE\n host={host}\n cpp ={cpp}")
    print(f"socket parity: {'FAIL' if failures else 'OK'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
