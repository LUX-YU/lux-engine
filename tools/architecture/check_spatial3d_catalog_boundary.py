#!/usr/bin/env python3
"""Guard Engine-owned Spatial3D scene-catalog contracts."""

from __future__ import annotations

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".inl", ".ipp", ".cmake", ".txt"}
LEGACY_INCLUDE = "lux/engine/resource/spatial3d_scene"
LEGACY_NAMESPACE = "lux::spatial3d_scene"
ALLOWED_LEGACY = {
    Path("engine/spatial3d/test/scene_catalog_contract_test.cpp"),
}


def read(rel: Path) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def scan_engine_consumers() -> list[str]:
    violations: list[str] = []
    for root in (ROOT / "engine", ROOT / "ecs", ROOT / "extensions"):
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            rel = path.relative_to(ROOT)
            if rel in ALLOWED_LEGACY:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            if LEGACY_INCLUDE in text:
                violations.append(f"{rel}: legacy Spatial3D Resource include")
            if LEGACY_NAMESPACE in text:
                violations.append(f"{rel}: legacy Spatial3D Resource namespace")
    return violations


def scan_contract() -> list[str]:
    violations: list[str] = []
    header = Path("engine/spatial3d/include/lux/engine/spatial3d/SceneCatalog.hpp")
    text = read(header)
    for token in (
        "namespace lux::spatial3d",
        "lux::scene::DemandChannelId demand_channel;",
        "lux::ecs::scene_format::EntitySectionId section;",
        "SceneCatalogResult<std::vector<std::byte>> encodeSceneCatalog",
        "SceneCatalogResult<SceneCatalog> decodeSceneCatalog",
    ):
        if token not in text:
            violations.append(f"{header}: missing canonical contract `{token}`")
    if LEGACY_INCLUDE in text or LEGACY_NAMESPACE in text:
        violations.append(f"{header}: public contract leaks legacy Resource API")
    return violations


def scan_runtime_and_toolchain() -> list[str]:
    violations: list[str] = []
    runtime = Path(
        "engine/runtime/spatial3d/partitioned/src/Spatial3DPartitionedContribution.cpp")
    runtime_text = read(runtime)
    for token in (
        "#include <lux/engine/spatial3d/SceneCatalog.hpp>",
        "lux::spatial3d::decodeSceneCatalog",
        "lux::spatial3d::SceneCatalog",
    ):
        if token not in runtime_text:
            violations.append(f"{runtime}: missing canonical token `{token}`")
    for token in (
        "lux::extensions::sameStableId",
        "Spatial3DSceneCatalogEntry",
        "legacySectionId",
        "legacyDemandChannel",
    ):
        if token in runtime_text:
            violations.append(f"{runtime}: stale cross-domain token `{token}`")

    toolchain = Path(
        "engine/toolchain/spatial3d_scene/src/Spatial3DEntitySceneAdapter.cpp")
    toolchain_text = read(toolchain)
    for token in (
        "lux::spatial3d::SceneCatalog",
        "lux::spatial3d::encodeSceneCatalog",
    ):
        if token not in toolchain_text:
            violations.append(f"{toolchain}: missing canonical token `{token}`")
    for token in ("legacySectionId", "legacyDemandChannel"):
        if token in toolchain_text:
            violations.append(f"{toolchain}: stale legacy adapter `{token}`")
    return violations


def scan_targets() -> list[str]:
    violations: list[str] = []
    root_cmake = Path("engine/CMakeLists.txt")
    if "add_subdirectory(spatial3d)" not in read(root_cmake):
        violations.append(f"{root_cmake}: Engine Spatial3D target is not configured")

    catalog_cmake = Path("engine/spatial3d/CMakeLists.txt")
    catalog_text = read(catalog_cmake)
    production = catalog_text.split("add_executable(", maxsplit=1)[0]
    if "lux::engine::resource::spatial3d_scene" in production:
        violations.append(
            f"{catalog_cmake}: canonical target depends on legacy Resource catalog")
    for token in (
        "spatial3d_scene_catalog_public_link_test",
        "spatial3d_scene_catalog_contract_test",
    ):
        if token not in catalog_text:
            violations.append(f"{catalog_cmake}: missing test target `{token}`")

    for rel in (
        Path("engine/runtime/spatial3d/partitioned/CMakeLists.txt"),
        Path("engine/toolchain/spatial3d_scene/CMakeLists.txt"),
    ):
        text = read(rel)
        if "lux::engine::spatial3d::scene_catalog" not in text:
            violations.append(f"{rel}: missing Engine Spatial3D catalog target")
        production = text.split("add_executable(", maxsplit=1)[0]
        if "lux::engine::resource::spatial3d_scene" in production:
            violations.append(f"{rel}: production still links legacy Resource catalog")
    return violations


def main() -> int:
    violations = (
        scan_engine_consumers()
        + scan_contract()
        + scan_runtime_and_toolchain()
        + scan_targets()
    )
    if violations:
        print("Spatial3D scene-catalog boundary violations:", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1
    print("Spatial3D scene-catalog boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
