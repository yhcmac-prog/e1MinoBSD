#!/usr/bin/env python3
"""Extract payloads of the offline closure into one merged tree.

Reads build/packages/pkg-list.txt and build/packages/All/*.pkg, merges all
payloads into a destination directory (default build/deskstage).
"""
import argparse
import hashlib
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG_DIR = os.path.join(ROOT, "build", "packages", "All")

# macOS strips the setuid/setgid bits when unprivileged users extract the
# packages, and mkisofs -gid 0 flattens all group ownership. Re-apply the
# special modes here (dbus helper also needs an other-execute bit so the
# messagebus user can invoke it once its group is root).
SPECIAL_MODES = {
    "usr/local/libexec/ck-get-x11-display-device": 0o4755,
    "usr/local/libexec/dbus-daemon-launch-helper": 0o4751,
    "usr/local/libexec/gstreamer-1.0/gst-ptp-helper": 0o4755,
    "usr/local/libexec/libgtop_server2": 0o2555,
    "usr/local/bin/mate_pam_helper": 0o4555,
    "usr/local/bin/pkexec": 0o4755,
    "usr/local/lib/polkit-1/polkit-agent-helper-1": 0o4755,
    "usr/local/libexec/Xorg.wrap": 0o4755,
}


def members(pkg):
    out = subprocess.run(["tar", "tf", pkg], check=True,
                         capture_output=True).stdout.decode()
    return [m for m in out.splitlines() if m]


def extract(pkg, dest):
    ms = members(pkg)
    payload = next((m for m in ms if m.startswith("payload")), None)
    if payload:
        # payload member is a (usually zstd) tar stream
        data = subprocess.run(["tar", "-xOf", pkg, payload], check=True,
                              capture_output=True).stdout
        subprocess.run(["tar", "--no-same-owner", "--no-xattrs", "--no-acls",
                        "--no-fflags", "-xmf", "-", "-C", dest],
                       input=data, check=True)
    else:
        # new-style compact package: staged tree at the archive root
        subprocess.run(["tar", "--no-same-owner", "--no-xattrs", "--no-acls",
                        "--no-fflags", "-xf", pkg, "-C", dest,
                        "--exclude=+MANIFEST", "--exclude=+COMPACT_MANIFEST",
                        "--exclude=signature"], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "deskstage"))
    ap.add_argument("--listfile",
                    default=os.path.join(ROOT, "build", "packages",
                                         "pkg-list.txt"))
    ap.add_argument("--no-marker", action="store_true",
                    help="do not write the completion marker")
    args = ap.parse_args()

    if not os.path.exists(args.listfile):
        sys.exit("pkg-list.txt missing; run fetch_pkgs.py first")
    raw = open(args.listfile).read()
    # columns: name, repo, path, version
    joblist = [(l.split("\t")[0], l.split("\t")[2])
               for l in raw.splitlines() if l.strip()]

    marker = os.path.join(args.out, ".extract-done")
    digest = hashlib.sha256(raw.encode()).hexdigest()
    if os.path.exists(marker) and open(marker).read().strip() == digest:
        print("desktop payload tree up to date")
        return

    os.makedirs(args.out, exist_ok=True)
    for i, (name, relpath) in enumerate(joblist, 1):
        fname = os.path.basename(relpath)
        pkg = os.path.join(PKG_DIR, fname)
        if not os.path.exists(pkg):
            sys.exit("package file missing for %s: %s" % (name, fname))
        print("[%d/%d] %s" % (i, len(joblist), fname), flush=True)
        extract(pkg, args.out)

    # Restore setuid/setgid bits stripped during host extraction.
    for rel, mode in SPECIAL_MODES.items():
        path = os.path.join(args.out, rel)
        if os.path.lexists(path):
            os.chmod(path, mode)

    if not args.no_marker:
        with open(marker, "w") as f:
            f.write(digest)
    print("desktop payload tree ready:", args.out)


if __name__ == "__main__":
    main()
