#!/usr/bin/env python3
"""Guard canonical EntitySection and ScenePackage ownership.

Pure LXES structures/codecs belong to ``lux::ecs::scene_format``. Engine-level
LXSC package policy belongs to ``lux::scene``. The historical
``modules/resource/entity_scene`` model remains reachable only through a small,
explicit compatibility boundary until all authoring consumers are migrated.
"""

from __future__ import annotations

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".inl", ".ipp"}

# Exact files that intentionally translate the frozen v1 wire model. No whole
# runtime/toolchain directory is exempted.
LEGACY_COMPATIBILITY_FILES = {
    Path("engine/runtime/entity_scene/test/entity_section_wire_compatibility_test.cpp"),
    Path("engine/scene/package/src/LegacyEntitySceneAdapter.cpp"),
    Path("engine/scene/package/src/LegacyEntitySceneAdapter.hpp"),
    Path("engine/scene/package/src/ScenePackageCodec.cpp"),
    Path("engine/scene/package/test/scene_package_contract_test.cpp"),
    # Temporary bridge for modules/resource/spatial3d_scene. Only its local
    # conversion helpers may materialize legacy Section/Demand IDs.
    Path("engine/toolchain/spatial3d_scene/src/Spatial3DEntitySceneAdapter.cpp"),
}

LEGACY_INCLUDE = "lux/engine/resource/entity_scene"
LEGACY_PURE_TOKENS = (
    "lux::entity_scene::EntitySectionId",
    "lux::entity_scene::EntitySectionImage",
    "lux::entity_scene::EntitySectionRecord",
    "lux::entity_scene::EntitySectionAttachment",
    "lux::entity_scene::ContentBlobId",
    "lux::entity_scene::ContentBlobRef",
    "lux::entity_scene::ContentTypeId",
    "lux::entity_scene::PersistenceJournal",
    "lux::entity_scene::PersistenceJournalRecord",
    "lux::entity_scene::ComponentSchemaId",
)

CANONICAL_RUNTIME_HEADERS = (
    Path("engine/runtime/entity_scene/include/lux/engine/runtime/entity_scene/EntityBatchDecoder.hpp"),
    Path("engine/runtime/entity_scene/include/lux/engine/runtime/entity_scene/EntityBatchTypes.hpp"),
    Path("engine/runtime/entity_scene/include/lux/engine/runtime/entity_scene/PreparedEntityBatch.hpp"),
    Path("engine/runtime/entity_scene/include/lux/engine/runtime/entity_scene/SectionBlobStore.hpp"),
)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def relative(path: Path) -> Path:
    return path.relative_to(ROOT)


def scan_sources() -> list[str]:
    violations: list[str] = []
    roots = (
        ROOT / "engine" / "runtime",
        ROOT / "engine" / "toolchain" / "entity_scene",
    )
    for base in roots:
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            rel = relative(path)
            if rel in LEGACY_COMPATIBILITY_FILES:
                continue
            try:
                lines = read(path).splitlines()
            except UnicodeDecodeError:
                continue
            for line_number, line in enumerate(lines, start=1):
                if LEGACY_INCLUDE in line:
                    violations.append(
                        f"{rel}:{line_number}: legacy Resource EntityScene include")
                for token in LEGACY_PURE_TOKENS:
                    if token in line:
                        violations.append(f"{rel}:{line_number}: {token}")
    return violations


def scan_runtime_target() -> list[str]:
    violations: list[str] = []
    path = ROOT / "engine" / "runtime" / "entity_scene" / "CMakeLists.txt"
    text = read(path)
    production = text.split("# Temporary migration gate:", maxsplit=1)[0]
    required = (
        "lux::engine::ecs::scene_format",
        "lux::engine::scene::scene_package",
    )
    for token in required:
        if token not in production:
            violations.append(f"{relative(path)}: missing production dependency `{token}`")
    if "lux::engine::resource::entity_scene" in production:
        violations.append(
            f"{relative(path)}: production runtime_entity_scene still links legacy Resource EntityScene")
    return violations


def scan_toolchain_target() -> list[str]:
    violations: list[str] = []
    path = ROOT / "engine" / "toolchain" / "entity_scene" / "CMakeLists.txt"
    text = read(path)
    for token in (
        "lux::engine::ecs::scene_format",
        "lux::engine::scene::scene_package",
    ):
        if token not in text:
            violations.append(f"{relative(path)}: missing canonical dependency `{token}`")
    if "lux::engine::resource::entity_scene" in text:
        violations.append(
            f"{relative(path)}: canonical cooker links legacy Resource EntityScene")
    return violations


def scan_public_contracts() -> list[str]:
    violations: list[str] = []
    for rel in CANONICAL_RUNTIME_HEADERS:
        path = ROOT / rel
        text = read(path)
        if "lux::ecs::scene_format" not in text:
            violations.append(f"{rel}: does not expose canonical ECS Scene Format types")
        if LEGACY_INCLUDE in text or "lux::entity_scene" in text:
            violations.append(f"{rel}: leaks legacy Resource EntityScene types")

    cooker = ROOT / "engine" / "toolchain" / "entity_scene" / "include" / \
        "lux" / "engine" / "toolchain" / "entity_scene" / "EntitySceneCooker.hpp"
    cooker_text = read(cooker)
    for token in ("ScenePackageCookInput", "CookedScenePackageBundle", "cookScenePackage"):
        if token not in cooker_text:
            violations.append(f"{relative(cooker)}: missing canonical contract `{token}`")
    if LEGACY_INCLUDE in cooker_text or "lux::entity_scene" in cooker_text:
        violations.append(f"{relative(cooker)}: public cooker leaks legacy Resource DTO")
    return violations


def scan_compatibility_scope() -> list[str]:
    """Ensure the temporary Spatial3D exception remains a tiny adapter."""
    violations: list[str] = []
    rel = Path("engine/toolchain/spatial3d_scene/src/Spatial3DEntitySceneAdapter.cpp")
    text = read(ROOT / rel)
    allowed_tokens = (
        "lux::entity_scene::DemandChannelId",
        "lux::entity_scene::EntitySectionId",
    )
    for token in LEGACY_PURE_TOKENS:
        if token in text and token not in allowed_tokens:
            violations.append(f"{rel}: unexpected legacy pure type `{token}`")
    if text.count(LEGACY_INCLUDE) > 1:
        violations.append(f"{rel}: legacy include spread beyond one local adapter include")
    return violations


def main() -> int:
    violations = (
        scan_sources()
        + scan_runtime_target()
        + scan_toolchain_target()
        + scan_public_contracts()
        + scan_compatibility_scope()
    )
    if violations:
        print("EntitySection ownership boundary violations:", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1
    print("EntitySection ownership boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
