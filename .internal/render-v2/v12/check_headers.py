"""Source probe validation; compiled and relocated consumers remain separate gates."""
import argparse
from pathlib import Path
import re


def closure(root, probe):
    headers = {}
    for p in (root / 'modules').rglob('*.hpp'):
        if 'include' in p.parts:
            headers['/'.join(p.parts[p.parts.index('include') + 1:])] = p
    if probe not in headers:
        raise ValueError(f'Public probe does not exist: {probe}')
    seen, pending = set(), [probe]
    while pending:
        key = pending.pop()
        if key in seen:
            continue
        seen.add(key)
        text = headers[key].read_text(encoding='utf-8-sig')
        pending.extend(k for k in re.findall(r'#include\s*[<"]([^>"\n]+)', text) if k in headers)
    return sorted(seen)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--probe', required=True)
    args = parser.parse_args()
    for name in closure(args.source.resolve(), args.probe):
        print(name)
