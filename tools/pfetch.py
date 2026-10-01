#!/usr/bin/env python3
"""pfetch — parallel ranged HTTP downloader (curl backend).

Usage: pfetch.py <url> [url...] -o <out> [--conn N] [--quiet]

Tries the given URLs in order (mirror fallback). Each segment is
downloaded independently with Range requests via curl and retried on
failure; completed segments (<out>.partN) are reused on rerun.
"""
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor


def curl(args, retries=3):
    for attempt in range(retries):
        r = subprocess.run(["curl", "-fsS", "--max-time", "600"] + args,
                           capture_output=True)
        if r.returncode == 0:
            return True
        time.sleep(min(2 ** attempt, 10))
    return False


def probe(urls):
    """Return (size, url) using a 1-byte ranged request."""
    last = b""
    for base in urls:
        r = subprocess.run(
            ["curl", "-fsS", "-r", "0-0", "-D", "-", "-o", "/dev/null", base],
            capture_output=True)
        if r.returncode != 0:
            last = r.stderr
            continue
        for line in r.stdout.splitlines():
            line = line.decode("latin1").strip().lower()
            if line.startswith("content-range:") and "/" in line:
                return int(line.rsplit("/", 1)[1]), base
    raise SystemExit("pfetch: all mirrors failed: %s" % last.decode())


def fetch_segment(base, start, end, part, retries=5):
    want = end - start + 1
    if os.path.exists(part) and os.path.getsize(part) == want:
        return
    for attempt in range(retries):
        pos = os.path.getsize(part) if os.path.exists(part) else 0
        ok = curl(["-r", "%d-%d" % (start + pos, end),
                   "-o", part + ".new", base])
        if ok:
            mode = "ab" if pos else "wb"
            with open(part, mode) as w, open(part + ".new", "rb") as r:
                while True:
                    buf = r.read(1 << 20)
                    if not buf:
                        break
                    w.write(buf)
            os.remove(part + ".new")
            if os.path.getsize(part) == want:
                return
        if os.path.exists(part + ".new"):
            os.remove(part + ".new")
        time.sleep(min(2 ** attempt, 10))
    raise SystemExit("pfetch: segment failed: %s bytes %d-%d"
                     % (part, start, end))


def main():
    args = sys.argv[1:]
    out = None
    conn = 8
    quiet = False
    urls = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-o":
            out = args[i + 1]
            i += 2
        elif a == "--conn":
            conn = int(args[i + 1])
            i += 2
        elif a == "--quiet":
            quiet = True
            i += 1
        else:
            urls.append(a)
            i += 1
    if not urls or not out:
        raise SystemExit(__doc__)

    size, base = probe(urls)
    if os.path.exists(out) and os.path.getsize(out) == size:
        if not quiet:
            print("pfetch: %s already complete (%d bytes)" % (out, size))
        return

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    seg = (size + conn - 1) // conn
    bounds = []
    n = 0
    for s in range(0, size, seg):
        bounds.append((s, min(s + seg - 1, size - 1),
                       "%s.part%d" % (out, n)))
        n += 1

    with ThreadPoolExecutor(max_workers=conn) as ex:
        futs = [ex.submit(fetch_segment, base, s, e, p)
                for s, e, p in bounds]
        done = 0
        for f in futs:
            f.result()
            done += 1
            if not quiet:
                print("pfetch: %d/%d segments (%s)" % (done, len(bounds), out))

    tmp = out + ".tmp"
    with open(tmp, "wb") as w:
        for _, _, p in bounds:
            with open(p, "rb") as r:
                while True:
                    buf = r.read(1 << 22)
                    if not buf:
                        break
                    w.write(buf)
    if os.path.getsize(tmp) != size:
        raise SystemExit("pfetch: assembled size mismatch")
    os.replace(tmp, out)
    for _, _, p in bounds:
        os.remove(p)
    if not quiet:
        print("pfetch: done %s (%d bytes)" % (out, size))


if __name__ == "__main__":
    main()
