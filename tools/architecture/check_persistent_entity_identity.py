#!/usr/bin/env python3
"""Enforce ECS ownership of runtime persistent-entity identity.

LXSC/LXES v1 compatibility data may still use the historical
``lux::entity_scene`` UUID wrappers. Runtime ECS state must use
``lux::ecs::PersistentEntityId`` / ``PersistentEntityRef`` and perform an
explicit value conversion at the wire boundary.
"""

from __future__ import annotations

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".inl", ".ipp"}
RUNTIME_ROOTS = (ROOT / "ecs", ROOT / "engine" / "runtime")

ALLOWED_LEGACY_FILES = {
    # Tests which construct the frozen wire DTO deliberately.
    Path("engine/runtime/entity_scene/test/entity_section_wire_compatibility_test.cpp"),
    Path("engine/runtime/entity_scene/test/runtime_entity_scene_integration_test.cpp"),
    Path("engine/runtime/spatial2d/infinite/test/infinite2d_headless_test.cpp"),
    Path("engine/runtime/spatial3d/partitioned/test/spatial3d_scene_runtime_test.cpp"),
}

EXPLICIT_RUNTIME_ADAPTERS = {
    Path("engine/runtime/entity_scene/src/EntityBatchInternal.hpp"),
    Path("engine/runtime/spatial2d/infinite/src/Infinite2DPixelContent.cpp"),
}

LEGACY_RUNTIME_TOKENS = (
    "lux::entity_scene::PersistentEntityId",
    "lux::entity_scene::PersistentEntityRef",
)

CORE_FORBIDDEN_TOKENS = (
    "lux::entity_scene",
    "lux/engine/resource/entity_scene",
    "lux::engine::resource::entity_scene",
)


def relative(path: Path) -> Path:
    return path.relative_to(ROOT)


def scan_runtime_identity() -> list[str]:
    violations: list[str] = []
    for base in RUNTIME_ROOTS:
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            rel = relative(path)
            if rel in ALLOWED_LEGACY_FILES:
                continue
            try:
                lines = path.read_text(encoding="utf-8").splitlines()
            except UnicodeDecodeError:
                continue
            for line_number, line in enumerate(lines, start=1):
                for token in LEGACY_RUNTIME_TOKENS:
                    if token not in line:
                        continue
                    # A production wire producer may construct the legacy UUID
                    # only from an ECS ID's value. Keep this exception exact and
                    # local instead of allowing a whole runtime directory.
                    window = "\n".join(lines[max(0, line_number - 2):line_number + 3])
                    if rel in EXPLICIT_RUNTIME_ADAPTERS and ".value()" in window:
                        continue
                    violations.append(f"{rel}:{line_number}: {token}")
    return violations


def scan_ecs_core() -> list[str]:
    violations: list[str] = []
    core = ROOT / "ecs" / "core"
    for path in core.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix not in SOURCE_SUFFIXES and path.name != "CMakeLists.txt":
            continue
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            continue
        rel = relative(path)
        for line_number, line in enumerate(lines, start=1):
            for token in CORE_FORBIDDEN_TOKENS:
                if token in line:
                    violations.append(f"{rel}:{line_number}: {token}")
    return violations


def scan_explicit_adapter() -> list[str]:
    """Keep the Authoring↔ECS conversion visible and typed."""
    path = ROOT / "engine/editor/src/scene/WorldActorEcsAdapter.cpp"
    text = path.read_text(encoding="utf-8")
    required = (
        "toAuthoringId(const lux::ecs::PersistentEntityId& id)",
        "toRuntimeId(const lux::entity_scene::PersistentEntityId& id)",
        "toRuntimeId(document.actor)",
        "toAuthoringId(stable->id())",
    )
    return [f"{relative(path)}: missing explicit boundary `{token}`"
            for token in required if token not in text]


def main() -> int:
    violations = (
        scan_runtime_identity()
        + scan_ecs_core()
        + scan_explicit_adapter()
    )
    if violations:
        print("Persistent entity identity boundary violations:", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1

    print("Persistent entity identity boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
