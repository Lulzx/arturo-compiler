#!/usr/bin/env python3
"""Generate runtime/info_data.inc from the pinned host's own `info.get`
documentation database. Run with the host binary on PATH (or ARTURO_HOST):

    ARTURO_HOST=arturo-bin/arturo python3 cpp26/tools/gen_info.py

The generated table is committed; re-run after an Arturo bump."""
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HOST = os.environ.get("ARTURO_HOST", "arturo")

DUMP = r"""
getOr: function [d k][ if key? d k -> return get d k return "" ]
dumpEntry: function [nm d][
    print "@@ENTRY"
    print nm
    print "@@keys"
    print keys d
    print "@@desc"
    print getOr d "description"
    print "@@module"
    print getOr d "module"
    print "@@type"
    print getOr d "type"
    print "@@line"
    print getOr d "line"
    print "@@source"
    print getOr d "source"
    print "@@args"
    a: getOr d "args"
    print size a
    loop keys a [k][
        print k
        print get a k
    ]
    print "@@attrs"
    t: getOr d "attrs"
    print size t
    loop keys t [k][
        print k
        s: get t k
        print keys s
        print getOr s "types"
        print getOr s "description"
    ]
    print "@@infix"
    print getOr d "infix?"
    print "@@returns"
    print getOr d "returns"
    print "@@example"
    e: getOr d "example"
    print size e
    loop e [line][ print "@@EXITEM" print line ]
    print "@@END"
]
"""


def main():
    src = (ROOT / "src/intrinsics.art").read_text()
    names = sorted(set(re.findall(r'"((?:[^"\\]|\\.)*)": #\[arity:', src)))
    assert len(names) == 395, len(names)
    lines = [DUMP]
    for nm in names:
        esc = nm.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(
            f'if throws? [ x: info.get \'{esc} dumpEntry "{esc}" x ] '
            f'[ print "@@SKIP" print "{esc}" ]')
    prog = ROOT / "cpp26/tests/info_dump.art"
    prog.write_text("\n".join(lines) + "\n")
    try:
        out = subprocess.run([HOST, "--no-color", str(prog)],
                             capture_output=True, text=True, timeout=300)
    finally:
        prog.unlink()
    if out.returncode != 0:
        print(f"host dump failed: {out.stderr[:500]}", file=sys.stderr)
        return 1
    entries = {}
    cur = None
    field = None
    buf = []

    def flush():
        if cur is not None and field:
            cur[field] = buf

    skipped = []
    pending_skip = False
    for line in out.stdout.split("\n"):
        if line == "@@SKIP":
            pending_skip = True
            continue
        if pending_skip:
            skipped.append(line)
            pending_skip = False
            continue
        if line == "@@ENTRY":
            flush()
            cur = {}
            entries[len(entries)] = cur
            field = "name"
            buf = []
        elif line == "@@EXITEM":
            buf.append(line)
        elif line.startswith("@@"):
            flush()
            field = line[2:].lower()
            buf = []
        else:
            buf.append(line)
    flush()

    def one(e, k):
        v = e.get(k, [""])
        return v[0] if v else ""

    def block_types(text):
        text = text.strip()
        if text.startswith("["):
            text = text[1:]
        if text.endswith("]"):
            text = text[:-1]
        return [t.lstrip(":") for t in text.split() if t]

    rows = []
    by_name = {}
    for e in entries.values():
        nm = one(e, "name")
        by_name[nm] = e
    missing = [n for n in names if n not in by_name and n not in skipped]
    if missing:
        print(f"missing entries: {missing[:10]}", file=sys.stderr)
        return 1
    print(f"skipped (host has no info): {skipped}", file=sys.stderr)
    for nm in names:
        if nm not in by_name:
            continue
        e = by_name[nm]
        args_raw = e.get("args", ["0"])
        nargs = int(args_raw[0])
        args = []
        idx = 1
        for _ in range(nargs):
            args.append((args_raw[idx], block_types(args_raw[idx + 1])))
            idx += 2
        attrs_raw = e.get("attrs", ["0"])
        nattrs = int(attrs_raw[0])
        attrs = []
        idx = 1
        for _ in range(nattrs):
            key = attrs_raw[idx]
            subkeys = attrs_raw[idx + 1].split()
            types = block_types(attrs_raw[idx + 2])
            desc = attrs_raw[idx + 3]
            assert set(subkeys) <= {"types", "description"}, (nm, key, subkeys)
            attrs.append((key, types, desc))
            idx += 4
        ex_raw = e.get("example", ["0"])
        nex = int(ex_raw[0])
        # items are delimited by @@EXITEM lines; an item may span lines
        items = []
        cur = None
        for ln in ex_raw[1:]:
            if ln == "@@EXITEM":
                if cur is not None:
                    items.append(cur)
                cur = ""
            elif cur is not None:
                cur = cur + "\n" + ln if cur else ln
        if cur is not None:
            items.append(cur)
        example = items[:nex]
        rows.append({
            "name": nm,
            "desc": one(e, "desc"),
            "module": one(e, "module"),
            "type": one(e, "type").lstrip(":"),
            "line": int(one(e, "line") or 0),
            "source": one(e, "source"),
            "args": args,
            "attrs": attrs,
            "returns": block_types(one(e, "returns")),
            "keys": one(e, "keys"),
            "infix": one(e, "infix"),
            "example": example,
        })

    def esc(s):
        return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'

    # capture the host's own panel description lines (the breaks are
    # fixed in the host's documentation data, not a runtime wrap)
    plines = []
    for nm in names:
        if nm not in by_name:
            continue
        esc2 = nm.replace("\\", "\\\\").replace('"', '\\"')
        plines.append(f"info '{esc2}")
    pprog = ROOT / "cpp26/tests/info_panels.art"
    pprog.write_text("\n".join(plines) + "\n")
    try:
        pout = subprocess.run([HOST, "--no-color", str(pprog)],
                              capture_output=True, text=True, timeout=300)
    finally:
        pprog.unlink()
    sep = "|" + "-" * 80
    pdesc = {}
    palias = {}
    adesc = {}
    ulines = {}
    rlines = {}
    cur = []
    panels = []
    for ln in pout.stdout.split("\n"):
        if ln == sep and cur and cur[-1] == sep:
            panels.append(cur)
            cur = []
        cur.append(ln)
    panels.append(cur)
    import re as _re
    for p in panels:
        if len(p) < 4:
            continue
        m = _re.match(r"\|\s+(\S+)  :", p[1])
        if not m:
            continue
        try:
            i1 = p.index(sep, 2)
            i2 = p.index(sep, i1 + 1)
        except ValueError:
            continue
        pdesc[m.group(1)] = "\n".join(ln[18:] for ln in p[i1 + 1:i2])
        if "alias" in p[2]:
            palias[m.group(1)] = p[2].split("alias", 1)[1].strip()
        # options section: capture each attr's description lines
        ent_attrs = {}
        cur_attr = None
        desc_col = None
        for ln in p:
            if " options  " in ln or (cur_attr and ln.startswith("|") and "returns" not in ln and ln != "|"):
                seg = ln
                if " options  " in seg:
                    seg = seg.split(" options  ", 1)[1]
                body = seg.strip()
                if body.startswith("|"):
                    body = body[1:].lstrip()
                if body.startswith(".") and " -> " in body:
                    left, desc_first = body.split(" -> ", 1)
                    aname = left[1:].split(" ")[0]
                    cur_attr = aname
                    desc_col = ln.index(desc_first)
                    ent_attrs[aname] = [desc_first]
                elif cur_attr:
                    ent_attrs[cur_attr].append(ln[desc_col - 7:])
            if " returns  " in ln:
                break
        if ent_attrs:
            adesc[m.group(1)] = ent_attrs
        # usage + returns sections, verbatim (without the leading '|')
        try:
            i2b = p.index(sep, i1 + 1) if i1 else -1
        except Exception:
            i2b = -1
        sec = p[i2 + 1:] if i2 >= 0 else []
        ul = []
        started = False
        for ln in sec:
            if " usage  " in ln:
                started = True
            elif started and (ln == "|" or "options" in ln or " returns  " in ln):
                break
            if started:
                ul.append(ln[1:])
        rl = []
        started = False
        for ln in sec:
            if " returns  " in ln:
                started = True
            elif started and ln == sep:
                break
            if started:
                rl.append(ln[1:])
        ulines[m.group(1)] = "\n".join(ul)
        rlines[m.group(1)] = "\n".join(rl)
    for r in rows:
        pd = pdesc.get(r["name"], "")
        if " ".join(pd.split()) != " ".join(r["desc"].split()):
            print(f"panel desc mismatch: {r['name']}", file=sys.stderr)
        r["pdesc"] = pd
        r["alias"] = palias.get(r["name"], "")
        ea = adesc.get(r["name"], {})
        r["adesc"] = "\x1f".join(
            k + "\x1e" + "\x1e".join(v) for k, v in ea.items())
        r["ulines"] = ulines.get(r["name"], "")
        r["rlines"] = rlines.get(r["name"], "")

    out_lines = [
        "/* GENERATED by cpp26/tools/gen_info.py from the pinned host's",
        " * `info.get` database (Arturo 0.10.0). Do not edit by hand. */",
        "typedef struct { const char *name; const char *desc; const char *module;",
        "                 const char *type; long line; const char *source;",
        "                 const char *args; const char *attrs; const char *returns;",
        "                 const char *example; const char *keys; const char *pdesc; const char *alias; const char *adesc; const char *ulines; const char *rlines; const char *infix; } InfoEntry;",
        "static const InfoEntry INFO_DB[] = {",
    ]
    for r in rows:
        args = ";".join(f"{k}={','.join(v)}" for k, v in r["args"])
        attrs = ";".join(f"{k}={','.join(t)}:{d}" for k, t, d in r["attrs"])
        out_lines.append(
            "  {%s, %s, %s, %s, %d, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s}," % (
                esc(r["name"]), esc(r["desc"]), esc(r["module"]), esc(r["type"]),
                r["line"], esc(r["source"]), esc(args), esc(attrs),
                esc(",".join(r["returns"])), esc("\x1e".join(r["example"])), esc(r["keys"]), esc(r["pdesc"]), esc(r["alias"]), esc(r["adesc"]), esc(r["ulines"]), esc(r["rlines"]), esc(r["infix"])))
    out_lines.append("};")
    out_lines.append(f"#define INFO_DB_COUNT {len(rows)}")
    (ROOT / "runtime/info_data.inc").write_text("\n".join(out_lines) + "\n")
    print(f"wrote runtime/info_data.inc ({len(rows)} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
