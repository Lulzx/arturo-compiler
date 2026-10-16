#!/usr/bin/env python3
"""Info parity: host interpreter vs compiled binary for the `info`
panel (all documented intrinsics), `info.get` dictionaries (address
excluded: it is a raw pointer in the host), user-function info, and
unknown names. Invoked from run_tests.sh; exits non-zero on divergence."""
import os
import pathlib
import re
import subprocess
import sys

CPP = pathlib.Path(__file__).resolve().parent.parent
ROOT = CPP.parent
ARTURO = os.environ.get("ARTURO_HOST", "arturo")
AC = CPP / "bin" / "arturoc"
WORK = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/arturoc-info")
WORK.mkdir(parents=True, exist_ok=True)

SKIP = {"cancel", "channel", "emit", "eval", "event", "failed?", "finished?",
        "off", "on", "sign", "unescape", "wait"}


def names():
    src = (ROOT / "src/intrinsics.art").read_text()
    return sorted(set(re.findall(r'"((?:[^"\\]|\\.)*)": #\[arity:', src)))


def main():
    all_names = [n for n in names() if n not in SKIP]
    get_prog = ["f: function [x y][x + y]", "d: info.get 'f",
                "loop keys d [k][ if k <> \"address\" -> print get d k ]"]
    for nm in all_names:
        esc = nm.replace("\\", "\\\\").replace('"', '\\"')
        get_prog.append(f'g: info.get \'{esc}')
        get_prog.append('loop keys g [k][ if k <> "address" -> print get g k ]')
    get_src = WORK / "infoget.art"
    get_src.write_text("\n".join(get_prog) + "\n")

    panel_prog = [f"info '{n.replace(chr(34), chr(92)+chr(34))}" for n in
                  ["print", "loop", "after", "call", "__VerbosePackager", "get", "serve"]]
    panel_prog.append("f: function [x y][x + y]")
    panel_prog.append("info 'f")
    panel_src = WORK / "infopanel.art"
    panel_src.write_text("\n".join(panel_prog) + "\n")

    bad_src = WORK / "infobad.art"
    bad_src.write_text("info 'nosuchthingxyz\n")

    failures = []
    for label, src, normalize_addr in [("get", get_src, False),
                                       ("panel", panel_src, True),
                                       ("bad", bad_src, False)]:
        host = subprocess.run([ARTURO, "--no-color", str(src)],
                              capture_output=True, text=True, timeout=120)
        binary = WORK / f"info{label}bin"
        build = subprocess.run([str(AC), str(src), str(binary)],
                               capture_output=True, text=True, timeout=120)
        if build.returncode != 0:
            failures.append(f"{label}: arturoc failed: {build.stderr[:300]}")
            continue
        cpp = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
        hout, cout = host.stdout, cpp.stdout
        if normalize_addr:
            hout = re.sub(r"\s+0x[0-9A-Fa-f]+", " ADDR", hout)
            cout = re.sub(r"\s+0x[0-9A-Fa-f]+", " ADDR", cout)
        if (hout, host.returncode) != (cout, cpp.returncode):
            failures.append(f"{label}: host=({host.returncode}) cpp=({cpp.returncode})")
            hl, cl = hout.split("\n"), cout.split("\n")
            for i in range(max(len(hl), len(cl))):
                a = hl[i] if i < len(hl) else "<EOF>"
                b = cl[i] if i < len(cl) else "<EOF>"
                if a != b:
                    failures.append(f"  first diff L{i}: host={a!r} cpp={b!r}")
                    break
        elif cpp.stderr and label != "bad":
            failures.append(f"{label}: cpp wrote to stderr: {cpp.stderr[:200]!r}")
    if failures:
        print("\n".join(failures))
        return 1
    print("info parity: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
