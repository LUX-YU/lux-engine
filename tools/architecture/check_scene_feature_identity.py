#!/usr/bin/env python3
"""Reject regressions that route Scene Feature identity through Extension IDs."""

from __future__ import annotations

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".inl", ".ipp"}

# These files implement the frozen LXSC v1 compatibility wire and are allowed
# to materialize the historical Extension-owned contribution ID explicitly.
LEGACY_WIRE_PREFIXES = (
    Path("modules/resource/entity_scene"),
    Path("engine/scene/package/src/LegacyEntitySceneAdapter.cpp"),
)

FORBIDDEN_TEXT = (
    "lux::extensions::contributionId(",
    "ContributionIdView legacy_id",
    "failure.contribution",
    "std::vector<lux::extensions::ContributionId>",
)


def is_legacy_wire(path: Path) -> bool:
    relative = path.relative_to(ROOT)
    return any(
        relative == prefix or prefix in relative.parents
        for prefix in LEGACY_WIRE_PREFIXES
    )


def main() -> int:
    violations: list[str] = []
    for path in ROOT.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        if ".git" in path.parts or is_legacy_wire(path):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for line_number, line in enumerate(text.splitlines(), start=1):
            for token in FORBIDDEN_TEXT:
                if token in line:
                    violations.append(
                        f"{path.relative_to(ROOT)}:{line_number}: {token}"
                    )

    if violations:
        print("Scene Feature identity boundary violations:", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1

    print("Scene Feature identity boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
