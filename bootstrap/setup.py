#!/usr/bin/env python3
"""
lux-engine 上游自举 —— 检查/拉取上游源码,再把构建交给 CMake Conductor。

## 为什么需要它

从零构建这个引擎要跨多个仓库按序进行:lux-cmake-toolset → {lux-cxx, imgui}
→ imgui-node-editor → lux-engine。顺序、每个仓库的 cmake 选项、装到哪个前缀,
此前一处都没写下来 —— 只存在于记忆里。代价是每次换机器、每次某个上游装陈旧
都要重新摸一遍(Android 就因为 install 前缀里的 lux-cxx 停在旧版本而整个起不来)。

顺序与选项在 lux_build.*.json 里(Conductor 的工程文件),仓库清单在
lux-manifest.json 里。本脚本只做三件事:**查**、**拉**、**转交**。它不重复
描述任何构建参数 —— 那样就会有第二处真相。

## 用法

    python bootstrap/setup.py status                     # 缺什么,一目了然
    python bootstrap/setup.py fetch                      # 拉缺失的仓库到同级目录
    python bootstrap/setup.py deps --platform host       # 安装固定 vcpkg 包
    python bootstrap/setup.py fetch --dest D:/src        # 拉到指定目录
    python bootstrap/setup.py build --platform host      # 转交 Conductor
    python bootstrap/setup.py build --platform android

`build` 后面多余的参数原样透传给 Conductor,例如:

    python bootstrap/setup.py build --platform host -- --stage configure --var lux_root=D:/src
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "lux-manifest.json"

# Conductor 的工程文件。平台名 → 文件名。
CONFIGS = {
    "host": HERE / "lux_build.host.json",
    "android": HERE / "lux_build.android.json",
}

# 默认的上游源码根:与 lux-engine 同级。与 lux_build.*.json 里 lux_root 的默认
# 值必须一致 —— 那边是 {config_dir}/../..,这里是 HERE/../..,同一个位置。
DEFAULT_SRC_ROOT = HERE.parent.parent


# ---------------------------------------------------------------------------
# 输出
# ---------------------------------------------------------------------------

def _ok(msg: str) -> str:
    return f"  [ok]      {msg}"


def _miss(msg: str) -> str:
    return f"  [MISSING] {msg}"


def _warn(msg: str) -> str:
    return f"  [warn]    {msg}"


# ---------------------------------------------------------------------------
# 清单
# ---------------------------------------------------------------------------

def load_manifest() -> dict:
    if not MANIFEST.exists():
        sys.exit(f"找不到清单:{MANIFEST}")
    with MANIFEST.open(encoding="utf-8") as f:
        return json.load(f)


def repos_for(manifest: dict, platform: str | None) -> list[dict]:
    """清单里参与该平台的仓库;platform 为 None 时返回全部。"""
    out = []
    for repo in manifest.get("repos", []):
        if platform is None or platform in repo.get("platforms", []):
            out.append(repo)
    return out


def tools_for(manifest: dict, platform: str | None) -> list[dict]:
    out = []
    for tool in manifest.get("tools", []):
        plats = tool.get("platforms")
        if plats is None or platform is None or platform in plats:
            out.append(tool)
    return out


def packages_for(manifest: dict, platform: str | None) -> list[dict]:
    out = []
    for package in manifest.get("vcpkg_packages", []):
        plats = package.get("platforms")
        if plats is None or platform is None or platform in plats:
            out.append(package)
    return out


def vcpkg_triplet(platform: str) -> str:
    if platform == "android":
        return "arm64-android"
    override = os.environ.get("VCPKG_DEFAULT_TRIPLET", "").strip()
    if override:
        return override
    if sys.platform == "win32":
        return "x64-windows"
    if sys.platform == "darwin":
        return "x64-osx"
    return "x64-linux"


def installed_vcpkg_packages(root: Path) -> dict[tuple[str, str], str]:
    status = root / "installed" / "vcpkg" / "status"
    if not status.is_file():
        return {}
    result: dict[tuple[str, str], str] = {}
    fields: dict[str, str] = {}
    for line in status.read_text(encoding="utf-8", errors="replace").splitlines() + [""]:
        if line:
            key, separator, value = line.partition(":")
            if separator:
                fields[key] = value.strip()
            continue
        if fields.get("Status") == "install ok installed":
            name = fields.get("Package", "")
            triplet = fields.get("Architecture", "")
            if name and triplet:
                result[(name, triplet)] = fields.get("Version", "")
        fields = {}
    return result


def check_package(package: dict, platform: str) -> tuple[bool, str]:
    root_value = os.environ.get("VCPKG_ROOT", "").strip()
    if not root_value:
        return False, f"{package['name']}: VCPKG_ROOT 未设置"
    root = Path(root_value)
    triplet = vcpkg_triplet(platform)
    installed = installed_vcpkg_packages(root)
    actual = installed.get((package["name"], triplet))
    required = package["version"]
    if actual == required:
        return True, f"{package['name']}:{triplet} {actual}"
    if actual:
        return False, (
            f"{package['name']}:{triplet} 需要 {required}，当前 {actual}"
        )
    return False, (
        f"{package['name']}:{triplet} {required} 未安装 —— {package['why']}"
    )


# ---------------------------------------------------------------------------
# 检查
# ---------------------------------------------------------------------------

def check_tool(tool: dict) -> tuple[bool, str]:
    """返回 (是否就绪, 人能看懂的说明)。"""
    kind = tool.get("probe")
    name = tool["name"]

    if kind == "command":
        path = shutil.which(tool["command"])
        if path:
            return True, f"{name}: {path}"
        return False, f"{name}: 不在 PATH 上 —— {tool['why']}"

    if kind == "env_dir":
        env = tool["env"]
        value = os.environ.get(env, "").strip()
        if not value:
            return False, f"{name}: 环境变量 {env} 未设置 —— {tool['why']}"
        if not Path(value).is_dir():
            # 报出实际的值:设错路径比没设更难查,因为看起来"设了"
            return False, f"{name}: {env}={value} 指向的目录不存在 —— {tool['why']}"
        return True, f"{name}: {value}"

    return False, f"{name}: 清单里的 probe 类型无法识别({kind})"


def check_repo(repo: dict, src_root: Path) -> tuple[bool, str]:
    path = src_root / repo["name"]
    if not path.is_dir():
        return False, f"{repo['name']}: 不在 {path}"
    if not (path / ".git").exists():
        # 不是致命错误:用户可能有意放了一份非 git 的源码
        return True, f"{repo['name']}: {path}(不是 git 检出)"
    rev = _git(path, "rev-parse", "--short", "HEAD") or "?"
    dirty = " *有本地修改" if _git(path, "status", "--porcelain") else ""
    return True, f"{repo['name']}: {path} @ {rev}{dirty}"


def _git(cwd: Path, *args: str) -> str:
    try:
        r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else ""
    except OSError:
        return ""


def cmd_status(args) -> int:
    manifest = load_manifest()
    src_root = Path(args.dest).resolve() if args.dest else DEFAULT_SRC_ROOT
    platform = args.platform

    print(f"上游源码根 : {src_root}")
    print(f"平台       : {platform or '全部'}")

    print("\n工具链:")
    tools_ok = True
    for tool in tools_for(manifest, platform):
        ok, msg = check_tool(tool)
        print(_ok(msg) if ok else _miss(msg))
        tools_ok &= ok

    print("\n上游仓库:")
    repos_ok = True
    for repo in repos_for(manifest, platform):
        ok, msg = check_repo(repo, src_root)
        print(_ok(msg) if ok else _miss(msg))
        repos_ok &= ok

    packages_ok = True
    package_platforms = [platform] if platform else list(CONFIGS)
    print("\nvcpkg 包:")
    for package_platform in package_platforms:
        for package in packages_for(manifest, package_platform):
            ok, msg = check_package(package, package_platform)
            print(_ok(msg) if ok else _miss(msg))
            packages_ok &= ok

    print("\nConductor:")
    conductor_ok, msg = check_conductor()
    print(_ok(msg) if conductor_ok else _miss(msg))

    if tools_ok and repos_ok and packages_ok and conductor_ok:
        print("\n全部就绪。")
        return 0
    if not repos_ok:
        print("\n缺仓库 → 跑 `setup.py fetch` 拉取。")
    if not tools_ok:
        print("缺工具链 → 按上面每行末尾说明的用途自行安装/设置环境变量。")
    if not packages_ok:
        print("缺 vcpkg 包 → 跑 `setup.py deps --platform <host|android>`。")
    return 1


def check_conductor() -> tuple[bool, str]:
    try:
        import cmake_node_editor  # noqa: F401
        return True, f"cmake_node_editor: {Path(cmake_node_editor.__file__).parent}"
    except ImportError:
        return False, (
            "cmake_node_editor 不可导入 —— 它是构建编排器(CMake Conductor)。"
            "装它,或把它的 src/ 加进 PYTHONPATH"
        )


# ---------------------------------------------------------------------------
# 拉取
# ---------------------------------------------------------------------------

def cmd_fetch(args) -> int:
    manifest = load_manifest()
    src_root = Path(args.dest).resolve() if args.dest else DEFAULT_SRC_ROOT
    src_root.mkdir(parents=True, exist_ok=True)

    print(f"拉取到:{src_root}\n")
    failed = []
    for repo in repos_for(manifest, args.platform):
        path = src_root / repo["name"]
        if path.exists():
            # 已存在的检出一律不动。用户可能正在上面工作,而 checkout/reset
            # 会毁掉未提交的改动 —— 这个工具没有权限做那种决定。
            print(_ok(f"{repo['name']}: 已存在,跳过(不会改动已有检出)"))
            continue

        print(f"  clone {repo['name']} …")
        cmd = ["git", "clone", repo["url"], str(path)]
        if subprocess.run(cmd).returncode != 0:
            failed.append(repo["name"])
            print(_miss(f"{repo['name']}: clone 失败"))
            continue

        rev = repo.get("revision", "").strip()
        if rev:
            if subprocess.run(["git", "checkout", rev], cwd=path).returncode != 0:
                print(_warn(f"{repo['name']}: 检出 {rev} 失败,留在默认分支"))

    if failed:
        print(f"\n{len(failed)} 个仓库拉取失败:{', '.join(failed)}")
        return 1
    print("\n完成。")
    return 0


def cmd_deps(args) -> int:
    manifest = load_manifest()
    root_value = os.environ.get("VCPKG_ROOT", "").strip()
    if not root_value:
        print(_miss("VCPKG_ROOT 未设置"))
        return 1
    root = Path(root_value)
    executable = root / ("vcpkg.exe" if sys.platform == "win32" else "vcpkg")
    if not executable.is_file():
        print(_miss(f"找不到 vcpkg 可执行文件: {executable}"))
        return 1
    triplet = vcpkg_triplet(args.platform)
    specifications = [
        f"{package['name']}:{triplet}"
        for package in packages_for(manifest, args.platform)
    ]
    if not specifications:
        print("该平台没有固定 vcpkg 包。")
        return 0
    command = [str(executable), "install", *specifications]
    if args.platform == "android":
        command.extend([
            "--overlay-triplets",
            str(HERE.parent / "cmake" / "triplets"),
        ])
    print(f"→ {' '.join(command)}\n")
    return subprocess.run(command).returncode


# ---------------------------------------------------------------------------
# 转交构建
# ---------------------------------------------------------------------------

def cmd_build(args) -> int:
    config = CONFIGS.get(args.platform)
    if config is None:
        sys.exit(f"未知平台:{args.platform}(可选:{', '.join(CONFIGS)})")
    if not config.exists():
        sys.exit(f"找不到构建描述:{config}")

    ok, msg = check_conductor()
    if not ok:
        sys.exit(msg)

    # 先跑一次检查,别让用户在编了十分钟之后才发现缺 NDK
    manifest = load_manifest()
    blockers = []
    for tool in tools_for(manifest, args.platform):
        tool_ok, tool_msg = check_tool(tool)
        if not tool_ok:
            blockers.append(tool_msg)
    for package in packages_for(manifest, args.platform):
        package_ok, package_msg = check_package(package, args.platform)
        if not package_ok:
            blockers.append(package_msg)
    if blockers:
        print("构建前检查未通过:")
        for b in blockers:
            print(_miss(b))
        return 1

    # argparse.REMAINDER 会把分隔用的 "--" 也收进来,原样转发的话 Conductor 会
    # 收到一个多余的 "--" 并报 unrecognized arguments。剥掉开头的那个。
    extra = list(args.extra)
    if extra and extra[0] == "--":
        extra = extra[1:]

    cmd = [sys.executable, "-m", "cmake_node_editor.cli", "build", str(config), *extra]
    print(f"→ {' '.join(cmd)}\n")
    return subprocess.run(cmd).returncode


# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="setup.py",
        description="lux-engine 上游自举:检查 / 拉取 / 转交构建",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = p.add_subparsers(dest="command", required=True)

    p_status = sub.add_parser("status", help="检查上游仓库与工具链是否就绪")
    p_status.add_argument("--dest", default=None, help="上游源码根(默认:与 lux-engine 同级)")
    p_status.add_argument("--platform", choices=list(CONFIGS), default=None)
    p_status.set_defaults(func=cmd_status)

    p_fetch = sub.add_parser("fetch", help="拉取缺失的上游仓库(已存在的不动)")
    p_fetch.add_argument("--dest", default=None, help="拉到哪(默认:与 lux-engine 同级)")
    p_fetch.add_argument("--platform", choices=list(CONFIGS), default=None)
    p_fetch.set_defaults(func=cmd_fetch)

    p_deps = sub.add_parser("deps", help="安装清单中固定版本的 vcpkg 包")
    p_deps.add_argument("--platform", choices=list(CONFIGS), required=True)
    p_deps.set_defaults(func=cmd_deps)

    p_build = sub.add_parser("build", help="转交 CMake Conductor 执行构建")
    p_build.add_argument("--platform", choices=list(CONFIGS), required=True)
    p_build.add_argument("extra", nargs=argparse.REMAINDER,
                         help="其余参数原样透传给 Conductor(如 --stage configure)")
    p_build.set_defaults(func=cmd_build)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
