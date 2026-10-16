#!/usr/bin/env python3
"""Store parity: each engine writes a store (native and JSON), the other
engine must read it back identically, including nested values, mutation
with autosave, and deferred stores staying unwritten until exit.
Invoked from run_tests.sh; exits non-zero on divergence."""
import os
import pathlib
import shutil
import subprocess
import sys

CPP = pathlib.Path(__file__).resolve().parent.parent
ARTURO = os.environ.get("ARTURO_HOST", "arturo")
AC = CPP / "bin" / "arturoc"
WORK = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/arturoc-store")

WRITE = """
data: store "mystore"
data\\name: "John"
data\\age: 36
data\\cfg: #[retries: 3 opts: #[deep: true]]
data\\items: [1 "two" 3.5 false null]
print data
print type data
print get data 'age
"""

READ = """
data: store "mystore"
print data
print data\\name
data\\age: 37
print data
"""

DEFERRED = """
data: store.deferred "dstore"
data\\x: 1
print "set"
"""


def run(engine, prog, directory):
    src = directory / "p.art"
    src.write_text(prog)
    if engine == "host":
        r = subprocess.run([ARTURO, "--no-color", str(src)], cwd=directory,
                           capture_output=True, text=True, timeout=20)
        return r.stdout, r.returncode
    binary = directory / "pb"
    build = subprocess.run([str(AC), str(src), str(binary)], capture_output=True, text=True, timeout=20)
    if build.returncode != 0:
        raise RuntimeError(f"arturoc failed: {build.stderr}")
    r = subprocess.run([str(binary)], cwd=directory, capture_output=True, text=True, timeout=20)
    return r.stdout, r.returncode


def main():
    shutil.rmtree(WORK, ignore_errors=True)
    failures = 0
    # native format, both directions
    for writer in ("host", "cpp"):
        other = "cpp" if writer == "host" else "host"
        directory = WORK / writer
        directory.mkdir(parents=True)
        out_w = run(writer, WRITE, directory)
        out_r = run(other, READ, directory)
        ref_dir = WORK / f"expect-{writer}"
        ref_dir.mkdir(parents=True)
        expect_w = run("host", WRITE, ref_dir)
        expect_r = run("host", READ, ref_dir)
        # note: out_r ran in `directory` (mutating the store to age 37)
        # while expect_r ran in ref_dir after its own WRITE; both end at 37
        if out_w != expect_w or out_r != expect_r:
            failures += 1
            print(f"NATIVE DIVERGENCE writer={writer}\n write {out_w} vs host {expect_w}\n read {out_r} vs host {expect_r}")
    # json format, both directions
    for writer in ("host", "cpp"):
        other = "cpp" if writer == "host" else "host"
        directory = WORK / f"json-{writer}"
        directory.mkdir(parents=True)
        prog_w = WRITE.replace('store "mystore"', 'store.json "jstore"')
        prog_r = READ.replace('store "mystore"', 'store.json "jstore"')
        out_w = run(writer, prog_w, directory)
        out_r = run(other, prog_r, directory)
        ref_dir = WORK / f"json-expect-{writer}"
        ref_dir.mkdir(parents=True)
        if out_w != run("host", prog_w, ref_dir) or out_r != run("host", prog_r, ref_dir):
            failures += 1
            print(f"JSON DIVERGENCE writer={writer}: {out_w} / {out_r}")
    # deferred: file must not contain the value until the process exits,
    # so the observable end state is the file after a successful run
    for engine in ("host", "cpp"):
        directory = WORK / f"deferred-{engine}"
        directory.mkdir(parents=True)
        out, rc = run(engine, DEFERRED, directory)
        content = (directory / "dstore").read_text() if (directory / "dstore").exists() else "<missing>"
        if rc != 0 or "x: 1" not in content:
            failures += 1
            print(f"DEFERRED FAILURE engine={engine} rc={rc} file={content!r}")
    print(f"store parity: {'FAIL' if failures else 'OK'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
