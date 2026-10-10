#!/usr/bin/env python3
"""Validate delivered Final Render V2 docs only (no Git/GPU qualification)."""
from __future__ import annotations
import csv
import hashlib
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APPENDIX = ROOT / "appendix"


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_csv(name):
    with (APPENDIX / name).open("r", encoding="utf-8-sig", newline="") as f:
        return list(csv.DictReader(f))


def main():
    chapters = [next(iter(ROOT.glob(f"{i:02d}_*.md")), None) for i in range(20)]
    check(all(chapters), "Missing one or more 00–19 authoritative chapters")
    for i, chapter in enumerate(chapters):
        data = chapter.read_text(encoding="utf-8")
        check(data.count("```") % 2 == 0, f"Unpaired code fences in {chapter.name}")
        check(len(data) > 2000 and data.startswith("# "), f"Incomplete chapter {chapter.name}")
        lines=data.splitlines()
        for k,line in enumerate(lines):
            if line.startswith("|") and k and lines[k-1].startswith("|"):
                check(line.count("|")==lines[k-1].count("|"), f"Malformed Markdown table {chapter.name}:{k+1}")
    rows = read_csv("LegacyCapabilityMatrix.csv")
    check(len(rows) >= 86, "Missing capability entries")
    ids=[r["Id"] for r in rows]
    check(len(ids)==len(set(ids)), "Duplicate capability IDs")
    for prefix,count in [('F',36),('I',44),('H',6)]:
        check(sum(k.startswith(prefix) for k in ids) == count, f"Missing legacy/extension {prefix} rows")
    check(sum(r['SourceSHA']=='a669409a289a6fa4092f21176397795b1cdb7f3e' for r in rows)==66,
          'Historical source rows changed')
    required = {
        "TypeOwnership.csv":("TypeOrFunction",127),
        "RequirementsTrace.csv":("RequirementId",51),
        "GoldenWorkloads.csv":("WorkloadId",25)
    }
    for name,(col,n) in required.items():
        r=read_csv(name)
        check(len(r)>=n and len({i[col] for i in r})==len(r),f"Missing or duplicate {name}")
    orig = list((ROOT / "_references_R2_readonly").glob("[0-9][0-9]_*.md"))
    check(len(orig)==12,"Historical R2 00–11 reference incomplete")
    manifest=ROOT/'MANIFEST.sha256'
    check(manifest.exists(),'Missing SHA manifest')
    entries={}
    for line in manifest.read_text(encoding='utf-8').splitlines():
        if not line.strip():continue
        digest,rel=line.split('  ',1)
        check(rel not in entries,'Duplicate manifest path')
        check(len(digest)==64 and re.fullmatch('[a-f0-9]{64}',digest),'Bad digest')
        entries[rel]=digest
        path=ROOT/rel
        check(path.is_file(),f'Missing {rel}')
        check(sha(path)==digest,f'Incorrect checksum {rel}')
    actual={f.relative_to(ROOT).as_posix() for f in ROOT.rglob('*') if f.is_file() and f != manifest}
    check(set(entries)==actual,f'File coverage mismatch: only_manifest={sorted(set(entries)-actual)}; only_tree={sorted(actual-set(entries))}')
    print(f'FINAL DOCUMENT PACKAGE PASS: {len(chapters)} chapters, {len(rows)} capabilities, '
          f'{len(actual)} hashed files, {len(orig)} original historical docs')
    print('NOTE: Package validation is NOT source/GPU or product qualification.')


if __name__=='__main__':
    main()
