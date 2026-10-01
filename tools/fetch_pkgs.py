#!/usr/bin/env python3
"""Resolve and download a dependency closure from FreeBSD quarterly + GhostBSD.

Usage:
  fetch_pkgs.py --out <dir> <seed> [<seed> ...]

Writes:
  <out>/All/*.pkg          package files
  <out>/pkg-list.txt       name <TAB> repo <TAB> path <TAB> version
"""
import argparse
import json
import os
import subprocess
import sys
import tarfile
from concurrent.futures import ThreadPoolExecutor

REPOS = {
    "freebsd": "https://mirrors.aliyun.com/freebsd-pkg/FreeBSD:14:amd64/quarterly",
    "ghostbsd": "https://pkg.ghostbsd.org/stable/FreeBSD:14:amd64/latest",
}


def load_site(repo, url, cache_dir):
    """Download packagesite.pkg and parse the JSON stream into {name: entry}."""
    raw = os.path.join(cache_dir, repo + ".pkg")
    if not os.path.exists(raw):
        print("[%s] fetching packagesite" % repo, flush=True)
        subprocess.run(["curl", "-sfSL", "--max-time", "300", "-o", raw,
                        url + "/packagesite.pkg"], check=True)
    data = {}
    out = subprocess.run(["tar", "-xOf", raw, "packagesite.yaml"],
                         check=True, capture_output=True).stdout.decode()
    for line in out.splitlines():
        line = line.strip()
        if line:
            j = json.loads(line)
            data[j["name"]] = j
    return data


def resolve(sites, name, origin_hint=None, prefer=None):
    """Pick an entry for package name across repos.

    Same-repo match first (when resolving deps of a chosen package), then
    match by dep origin, then any repo.
    """
    if prefer and name in sites[prefer]:
        return prefer, sites[prefer][name]
    if origin_hint:
        for repo, site in sites.items():
            e = site.get(name)
            if e and e.get("origin") == origin_hint:
                return repo, e
    for repo, site in sites.items():
        if name in site:
            return repo, site[name]
    return None, None


def closure(sites, seeds):
    """BFS over deps; returns list of (repo, entry) in deterministic order."""
    chosen = {}          # name -> (repo, entry)
    queue = []
    for s in seeds:
        repo, entry = resolve(sites, s)
        if entry is None:
            sys.exit("seed package not found: " + s)
        queue.append(s)
        chosen[s] = (repo, entry)
    while queue:
        name = queue.pop(0)
        repo, entry = chosen[name]
        for dep_name, dep_info in (entry.get("deps") or {}).items():
            if dep_name in chosen:
                continue
            drepo, dentry = resolve(sites, dep_name,
                                    origin_hint=dep_info.get("origin"),
                                    prefer=repo)
            if dentry is None:
                sys.exit("missing dependency %s (required by %s)"
                         % (dep_name, name))
            chosen[dep_name] = (drepo, dentry)
            queue.append(dep_name)
    return sorted(chosen.values(), key=lambda re: re[1]["name"]), chosen


def download_one(sites_meta, out_all):
    repo, entry = sites_meta
    rel = entry["path"].lstrip("/")
    dest = os.path.join(out_all, os.path.basename(rel))
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        return rel, True
    url = REPOS[repo] + "/" + rel
    subprocess.run(["curl", "-sfSL", "--retry", "6", "--retry-all-errors",
                    "--retry-delay", "2", "--max-time", "600",
                    "-o", dest + ".part", url], check=True)
    os.replace(dest + ".part", dest)
    return rel, False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--meta-cache", default=None)
    ap.add_argument("seeds", nargs="+")
    args = ap.parse_args()

    out_all = os.path.join(args.out, "All")
    os.makedirs(out_all, exist_ok=True)
    cache_dir = args.meta_cache or os.path.join(args.out, ".meta")
    os.makedirs(cache_dir, exist_ok=True)

    sites = {repo: load_site(repo, url, cache_dir) for repo, url in REPOS.items()}
    picked, _ = closure(sites, args.seeds)
    print("closure size: %d packages" % len(picked), flush=True)

    with ThreadPoolExecutor(max_workers=8) as pool:
        futures = [pool.submit(download_one, item, out_all) for item in picked]
        done = 0
        for fut in futures:
            rel, cached = fut.result()
            done += 1
            print("[%d/%d] %s%s" % (done, len(picked), rel,
                                    "" if not cached else " (cached)"), flush=True)

    with open(os.path.join(args.out, "pkg-list.txt"), "w") as f:
        for repo, entry in picked:
            f.write("%s\t%s\t%s\t%s\n"
                    % (entry["name"], repo, entry["path"], entry["version"]))
    print("pkg-list.txt written")


if __name__ == "__main__":
    main()
