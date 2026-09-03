#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import pathlib
import sys


def load_packager(path: pathlib.Path):
    specification = importlib.util.spec_from_file_location("lux_lua_packager", path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def expect_failure(action, text: str) -> None:
    try:
        action()
    except ValueError as error:
        assert text in str(error), (text, str(error))
        return
    raise AssertionError(f"expected failure containing: {text}")


def main() -> int:
    package = load_packager(pathlib.Path(sys.argv[1]))
    semantics = package.make_semantics([], ["lux.simulation.ScriptEndPlayReason,3,4,4"])
    delay = package.AbilitySchema("lux.simulation.delay", package.fnv1a("lux.simulation.delay"), 1, 17)
    abilities = {delay.contract: delay}

    def parse(source: str, symbols: dict[str, int], schemas=abilities):
        return package.collect_exports(source, "ENTITY", "Enemy", "lux.test.lua", semantics, symbols, schemas)

    expect_failure(
        lambda: parse(
            """---@lux.method
---@lux.lifecycle begin_play
---@return void
function Enemy:first() end
---@lux.method
---@lux.lifecycle begin_play
---@return void
function Enemy:second() end
""",
            {"Enemy:first": 1, "Enemy:second": 2},
        ),
        "duplicate BeginPlay",
    )
    expect_failure(
        lambda: parse(
            """---@lux.method
---@lux.lifecycle begin_play
---@param value lux.i32
---@return void
function Enemy:start(value) end
""",
            {"Enemy:start": 1},
        ),
        "BeginPlay lifecycle signature",
    )
    expect_failure(
        lambda: parse(
            """---@lux.method
---@lux.lifecycle end_play
---@param reason lux.u32
---@return void
function Enemy:finish(reason) end
""",
            {"Enemy:finish": 1},
        ),
        "EndPlay lifecycle signature",
    )
    expect_failure(
        lambda: parse(
            """---@lux.method
---@lux.lifecycle begin_play
---@lux.coroutine
---@return void
function Enemy:start() end
""",
            {"Enemy:start": 1},
        ),
        "lifecycle export cannot be coroutine",
    )
    expect_failure(
        lambda: parse(
            """---@lux.method
---@lux.coroutine
---@return lux.i32
function Enemy:update() return 1 end
""",
            {"Enemy:update": 1},
        ),
        "coroutine export must return void",
    )
    expect_failure(
        lambda: parse(
            """---@lux.requires lux.missing
---@lux.method
---@return void
function Enemy:update() end
""",
            {"Enemy:update": 1},
        ),
        "unknown Ability requirement",
    )
    expect_failure(
        lambda: parse(
            """---@lux.requires lux.simulation.delay
---@lux.requires lux.simulation.delay
---@lux.method
---@return void
function Enemy:update() end
""",
            {"Enemy:update": 1},
        ),
        "duplicate Ability requirement",
    )

    old = parse(
        """---@lux.method
---@lux.lifecycle begin_play
---@return void
function Enemy:initialize_old() end
""",
        {"Enemy:initialize_old": 91},
    )
    renamed = parse(
        """---@lux.method
---@lux.lifecycle begin_play
---@return void
function Enemy:initialize_new() end
""",
        {"Enemy:initialize_new": 91},
    )
    assert old.begin_play == renamed.begin_play == 91
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
