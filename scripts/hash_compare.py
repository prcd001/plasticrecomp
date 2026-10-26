"""Hash two disc copies of Army Men II and compare them with each other and with manifests.

Reads only. For every file under ENG and WebColl it records size, MD5 and SHA-256, then
compares by relative path (case-insensitive, `/`-separated):

  eng_vs_<label>  each manifest entry (a JSON list of {path, bytes, sha256}) against the
                  same path in ENG; a = manifest, b = ENG
  webcoll_vs_eng  WebColl file against ENG; WebColl keeps the game under ARMYMEN/, so that
                  prefix is stripped before matching (its AVI/, DirectX/, MPLAYNOW/ sit at
                  the disc root, as ENG's do); a = WebColl, b = ENG

Each row is `match`, `differ`, `only_a` or `only_b`. An unmatched file whose content
appears elsewhere on the other side gets that path in `same_content_at`. Writes one TSV
and prints a JSON summary; `--cache` keeps the per-tree hash lists (hashes only) so a
rerun does not reread the discs.

usage: python -I scripts/hash_compare.py --eng DIR --webcoll DIR
           --manifest LABEL=JSON [--manifest LABEL=JSON ...] --tsv OUT.tsv [--cache DIR]
"""
import argparse
import collections
import hashlib
import json
import os
import sys


def inventory(root):
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            sha, md5 = hashlib.sha256(), hashlib.md5()
            with open(full, "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    sha.update(block)
                    md5.update(block)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            out.append({"path": rel, "bytes": os.path.getsize(full),
                        "sha256": sha.hexdigest(), "md5": md5.hexdigest()})
    return out


def cached_inventory(root, cache_dir, name):
    if cache_dir:
        path = os.path.join(cache_dir, name + ".json")
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                return json.load(f)
    inv = inventory(root)
    if cache_dir:
        os.makedirs(cache_dir, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(inv, f, ensure_ascii=False, indent=0)
    return inv


def compare(label, a, b, key_a, key_b):
    """a, b: lists of entries. Rows are (label, status, a_path, b_path, a_bytes, b_bytes,
    a_sha, b_sha, same_content_at)."""
    by_key_b = {}
    for e in b:
        by_key_b.setdefault(key_b(e["path"]), e)
    sha_a = collections.defaultdict(list)
    sha_b = collections.defaultdict(list)
    for e in a:
        sha_a[e["sha256"]].append(e["path"])
    for e in b:
        sha_b[e["sha256"]].append(e["path"])
    rows, used = [], set()
    for e in a:
        o = by_key_b.get(key_a(e["path"]))
        if o is None or id(o) in used:
            rows.append((label, "only_a", e["path"], "", e["bytes"], "", e["sha256"], "",
                         ";".join(sha_b.get(e["sha256"], []))))
            continue
        used.add(id(o))
        status = "match" if o["sha256"] == e["sha256"] else "differ"
        rows.append((label, status, e["path"], o["path"], e["bytes"], o["bytes"],
                     e["sha256"], o["sha256"], ""))
    for e in b:
        if id(e) not in used:
            rows.append((label, "only_b", "", e["path"], "", e["bytes"], "", e["sha256"],
                         ";".join(sha_a.get(e["sha256"], []))))
    return rows


def summarise(rows):
    tally = collections.Counter()
    by_top = collections.defaultdict(collections.Counter)
    for r in rows:
        path = r[2] or r[3]
        top = path.split("/")[0] if "/" in path else "(root)"
        tally[r[1]] += 1
        by_top[top.lower()][r[1]] += 1
    return {"total": dict(tally), "by_top": {k: dict(v) for k, v in sorted(by_top.items())}}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eng", required=True)
    ap.add_argument("--webcoll", required=True)
    ap.add_argument("--manifest", action="append", required=True, help="LABEL=PATH")
    ap.add_argument("--tsv", required=True)
    ap.add_argument("--cache")
    args = ap.parse_args()

    eng = cached_inventory(args.eng, args.cache, "eng")
    web = cached_inventory(args.webcoll, args.cache, "webcoll")
    manifests = []
    for spec in args.manifest:
        label, _, path = spec.partition("=")
        with open(path, encoding="utf-8") as f:
            manifests.append((label, json.load(f)))

    lower = str.lower

    def strip_armymen(p):
        return p[len("ARMYMEN/"):].lower() if p.upper().startswith("ARMYMEN/") else p.lower()

    # Path-match WebColl's ARMYMEN/ copy first, so a root-level twin (Armymen2.ico) is extra.
    web_sorted = sorted(web, key=lambda e: not e["path"].upper().startswith("ARMYMEN/"))
    rows = []
    for label, man in manifests:
        rows += compare("eng_vs_" + label, man, eng, lower, lower)
    rows += compare("webcoll_vs_eng", web_sorted, eng, strip_armymen, lower)

    with open(args.tsv, "w", encoding="utf-8", newline="\n") as f:
        f.write("# eng_vs_<manifest>: a = manifest, b = ENG. webcoll_vs_eng: a = WebColl, "
                "b = ENG. only_a / only_b = present on that side only.\n")
        f.write("comparison\tstatus\ta_path\tb_path\ta_bytes\tb_bytes\ta_sha256\tb_sha256"
                "\tsame_content_at\n")
        for r in rows:
            f.write("\t".join(str(x) for x in r) + "\n")

    def tree(inv):
        return {"files": len(inv), "bytes": sum(e["bytes"] for e in inv)}

    summary = {"eng": tree(eng), "webcoll": tree(web)}
    for label, man in manifests:
        summary["manifest_" + label] = tree(man)
    for comp in sorted({r[0] for r in rows}):
        summary[comp] = summarise([r for r in rows if r[0] == comp])
    json.dump(summary, sys.stdout, indent=1)
    print()


if __name__ == "__main__":
    main()
