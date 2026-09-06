#!/usr/bin/env python3
"""hughes_bridge OTA manifest manager -- edit firmware.flensor.com/hughes.json without hand-editing JSON.

Run it on the Dreamhost server (or anywhere the hughes-*.bin files live):
    python3 manifest_tui.py [webdir]
Default webdir is ~/firmware.flensor.com. It auto-computes sha256 + size, backs up the old
manifest before writing, and validates that the bin the manifest points at actually exists + matches.
"""
import os, sys, json, hashlib, glob, re, shutil, datetime

BASEURL  = "https://firmware.flensor.com"
WEBDIR   = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.expanduser("~/firmware.flensor.com")
MANIFEST = os.path.join(WEBDIR, "hughes.json")

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()

def bins():
    return sorted(glob.glob(os.path.join(WEBDIR, "hughes-*.bin")))

def load_manifest():
    try:
        with open(MANIFEST) as f:
            return json.load(f)
    except Exception:
        return None

def ver_from_name(path):
    m = re.search(r'hughes-(.+)\.bin$', os.path.basename(path))
    return m.group(1) if m else ""

def show():
    m = load_manifest()
    print("\n=== current hughes.json ===")
    if not m:
        print("  (missing or invalid JSON)"); return
    for k in ("version", "url", "sha256", "size", "notes"):
        print("  %-8s: %s" % (k, m.get(k)))
    binname = os.path.basename(m.get("url", ""))
    binpath = os.path.join(WEBDIR, binname)
    if not os.path.exists(binpath):
        print("  !! WARNING: %s does NOT exist in the webdir" % binname)
    else:
        ok_sha = sha256(binpath) == m.get("sha256")
        ok_sz  = os.path.getsize(binpath) == m.get("size")
        print("  bin check: exists=yes  sha=%s  size=%s" %
              ("OK" if ok_sha else "MISMATCH", "OK" if ok_sz else "MISMATCH"))

def list_bins():
    print("\n=== firmware bins in %s ===" % WEBDIR)
    bs = bins()
    if not bs:
        print("  (none -- scp a hughes-<version>.bin here first)"); return bs
    cur = load_manifest(); curbin = os.path.basename(cur.get("url", "")) if cur else ""
    for i, b in enumerate(bs, 1):
        name = os.path.basename(b)
        mark = "  <- manifest points here" if name == curbin else ""
        print("  %2d. %-22s %8d bytes   v%s%s" % (i, name, os.path.getsize(b), ver_from_name(b), mark))
    return bs

def publish():
    bs = list_bins()
    if not bs:
        return
    try:
        sel = int(input("\npoint the manifest at which #  (0 = cancel): "))
    except ValueError:
        return
    if sel < 1 or sel > len(bs):
        print("cancelled."); return
    binpath = bs[sel - 1]; name = os.path.basename(binpath)
    ver   = ver_from_name(binpath)
    v     = input("version [%s]: " % ver).strip() or ver
    notes = input("notes [hughes_bridge %s]: " % v).strip() or ("hughes_bridge %s" % v)
    manifest = {"version": v, "url": "%s/%s" % (BASEURL, name),
                "sha256": sha256(binpath), "size": os.path.getsize(binpath), "notes": notes}
    print("\n=== new hughes.json ===")
    print(json.dumps(manifest))
    if input("\nwrite this to hughes.json? [y/N] ").strip().lower() != "y":
        print("cancelled."); return
    if os.path.exists(MANIFEST):
        bak = MANIFEST + "." + datetime.datetime.now().strftime("%Y%m%d_%H%M%S") + ".bak"
        shutil.copy2(MANIFEST, bak); print("backed up old manifest -> %s" % os.path.basename(bak))
    with open(MANIFEST, "w") as f:
        f.write(json.dumps(manifest) + "\n")
    print("done -- devices pointed at hughes.json will offer v%s on their next check." % v)

def menu():
    print("\nhughes_bridge manifest manager   (webdir: %s)" % WEBDIR)
    while True:
        print("\n  1) show current manifest + validate")
        print("  2) list firmware bins")
        print("  3) publish -- point the manifest at a bin (auto sha256/size)")
        print("  q) quit")
        try:
            c = input("> ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            print(); break
        if   c == "1": show()
        elif c == "2": list_bins()
        elif c == "3": publish()
        elif c in ("q", "quit", "exit"): break

if __name__ == "__main__":
    if not os.path.isdir(WEBDIR):
        print("webdir not found: %s  (pass it as an argument)" % WEBDIR); sys.exit(1)
    menu()
