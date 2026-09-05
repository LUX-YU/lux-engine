#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile


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
    schema_path = pathlib.Path(sys.argv[2])
    semantics = package.make_semantics([schema_path])
    assert "lux.i64" not in semantics and "lux.u64" not in semantics
    delay = package.AbilitySchema(
        "lux.simulation.delay",
        package.fnv1a("lux.simulation.delay"),
        "Delay",
        "Simulation Delay",
        1,
        17,
    )
    abilities = {delay.contract: delay}
    payload = semantics["lux.i32"]
    damage = package.EventSource(
        "Gameplay",
        "damage",
        11,
        12,
        0,
        payload,
        package.fnv1a("lux.i32"),
        1,
        14, 15, 1,
    )
    spawned = package.EventSource(
        "Gameplay",
        "spawned",
        11,
        13,
        0,
        payload,
        package.fnv1a("lux.i32"),
        1,
        14, 15, 1,
    )
    events = {
        (damage.system_name, damage.event_name): damage,
        (spawned.system_name, spawned.event_name): spawned,
    }

    with tempfile.TemporaryDirectory() as directory:
        manifest = pathlib.Path(directory) / "ability.json"
        document = {
            "schema": "lux-script-ability",
            "version": 2,
            "abilities": [
                {
                    "contract": delay.contract,
                    "contract_id": delay.contract_id,
                    "name": delay.name,
                    "display_name": delay.display_name,
                    "schema_version": delay.schema_version,
                    "schema_hash": delay.schema_hash,
                }
            ],
        }
        manifest.write_text(json.dumps(document), encoding="utf-8")
        assert package.load_ability_schemas([manifest])[delay.contract] == delay
        document["abilities"][0]["name"] = "invalid-name"
        manifest.write_text(json.dumps(document), encoding="utf-8")
        expect_failure(
            lambda: package.load_ability_schemas([manifest]),
            "invalid Ability schema",
        )

        event_manifest = pathlib.Path(directory) / "event.json"
        event_document = {
            "schema": "lux-script-event",
            "version": 2,
            "events": [
                {
                    "system_name": damage.system_name,
                    "event_name": damage.event_name,
                    "system_id": damage.system_id,
                    "event_id": damage.event_id,
                    "route": "simulation_broadcast",
                    "payload": {
                        "canonical_name": payload.canonical,
                        "type_id": payload.type_id,
                        "abi_kind": payload.abi_kind,
                        "size": payload.size,
                        "alignment": payload.alignment,
                    },
                    "payload_schema_hash": damage.payload_schema_hash,
                    "payload_schema_version": damage.payload_schema_version,
                    "delivery_hook_id": damage.delivery_hook_id,
                    "delivery_schema_hash": damage.delivery_schema_hash,
                    "delivery_schema_version": damage.delivery_schema_version,
                }
            ],
        }
        event_manifest.write_text(json.dumps(event_document), encoding="utf-8")
        assert package.load_event_schemas([event_manifest])[("Gameplay", "damage")] == damage
        conflicting_manifest = pathlib.Path(directory) / "event-conflict.json"
        event_document["events"][0]["event_id"] = damage.event_id + 1
        conflicting_manifest.write_text(json.dumps(event_document), encoding="utf-8")
        expect_failure(
            lambda: package.load_event_schemas([event_manifest, conflicting_manifest]),
            "conflicting Event schema",
        )
        event_document["events"][0]["payload_schema_hash"] = 0
        conflicting_manifest.write_text(json.dumps(event_document), encoding="utf-8")
        expect_failure(
            lambda: package.load_event_schemas([conflicting_manifest]),
            "invalid Event schema",
        )

    def parse(source: str, symbols: dict[str, int], schemas=abilities, event_schemas=events):
        return package.collect_exports(
            source,
            "ENTITY",
            "Enemy",
            "lux.test.lua",
            semantics,
            symbols,
            schemas,
            event_schemas,
        )

    for wide in ("lux.i64", "lux.u64"):
        expect_failure(lambda: parse(
            "---@lux.method\n---@param value " + wide + "\n---@return void\nfunction Enemy:update(value) end\n",
            {"Enemy:update": 1}), "unsupported")

    expect_failure(
        lambda: parse(
            """---@lux.event Gameplay.missing
---@lux.method
---@return void
function Enemy:update() end
""",
            {"Enemy:update": 1},
        ),
        "unknown Event source",
    )
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
    expect_failure(
        lambda: parse(
            """---@lux.event Gameplay.damage
---@lux.event Gameplay.damage
---@lux.method
---@return void
function Enemy:update() end
""",
            {"Enemy:update": 1},
        ),
        "duplicate Event source",
    )
    expect_failure(
        lambda: parse(
            """---@lux.method
---@return void
function Enemy:update() end
---@lux.event Gameplay.damage
""",
            {"Enemy:update": 1},
        ),
        "@lux.event must precede all exports",
    )

    event_sources = parse(
        """---@lux.event Gameplay.spawned
---@lux.event Gameplay.damage
---@lux.method
---@return void
function Enemy:update() end
""",
        {"Enemy:update": 1},
    )
    assert event_sources.event_sources == [
        damage,
        spawned,
    ]

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
