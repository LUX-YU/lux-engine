#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/script/backends/NativeModuleScriptBackend.hpp>
#include <lux/engine/ecs/script/systems/ScriptEventRegistry.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]] std::vector<std::byte> readFile(
        const std::filesystem::path& path
    )
    {
        std::ifstream input(path, std::ios::binary);
        assert(input);
        input.seekg(0, std::ios::end);
        const auto size = input.tellg();
        assert(size > 0);
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        );
        assert(input || input.eof());
        return bytes;
    }

    lux::rdesc::ScriptValueType valueType(
        const lux_script_type_desc& type
    )
    {
        return lux::rdesc::ScriptValueType{
            type.name ? type.name : "",
            type.type_id,
            type.size,
            type.align,
            type.kind
        };
    }

    lux::rdesc::ScriptFunction function(
        std::string name,
        std::span<const lux_script_type_desc> arguments = {}
    )
    {
        lux::rdesc::ScriptFunction result;
        result.name = std::move(name);
        for (const auto& argument : arguments)
            result.args.push_back(valueType(argument));
        return result;
    }

    lux::rdesc::Script description(
        lux::ecs::ScriptEventId pair_event,
        float default_value
    )
    {
        const auto& events = lux::ecs::scriptEventRegistry();
        lux::rdesc::NativeModuleScript native;
        native.abi_version = LUX_SCRIPT_ABI_VERSION;
        native.state_size = sizeof(float);
        native.state_defaults.resize(sizeof(float));
        std::memcpy(
            native.state_defaults.data(),
            &default_value,
            sizeof(default_value)
        );
        native.functions.push_back(function("Increment"));
        native.functions.push_back(function(
            "OnUpdate",
            events.desc(lux::ecs::ScriptEventRegistry::kOnUpdate).abi_params
        ));
        native.functions.push_back(function(
            "OnPair",
            events.desc(pair_event).abi_params
        ));

        lux::rdesc::Script script;
        script.module_name = "native-session-contract";
        script.body = std::move(native);
        return script;
    }
}

int main()
{
    auto& events = lux::ecs::scriptEventRegistry();
    const auto pair_event = events.registerEvent(
        "OnPair",
        {
            {&lux::meta::ref_type_of_v<float>, "value"},
            {&lux::meta::ref_type_of_v<std::uint32_t>, "count"}
        }
    );
    assert(pair_event != lux::ecs::kInvalidScriptEvent);

    const auto bytes = readFile(LUX_SCRIPT_NATIVE_FIXTURE);
    lux::ecs::World world;
    const auto entity = world.createEntity();
    const lux::ecs::EntityHandle self{world.registry(), entity};
    const auto id = uuids::uuid::from_string(
        "b4f07dc2-d0cd-407c-b81e-5840a1844c20"
    ).value();

    lux::ecs::NativeModuleScriptBackend backend;
    auto exact = description(pair_event, 4.0f);
    auto instance = backend.createInstanceFromAsset(
        self,
        world,
        exact,
        bytes,
        id,
        7
    );
    assert(instance);

    const auto update = instance.entry(
        lux::ecs::ScriptEventRegistry::kOnUpdate
    );
    assert(update.invoke != nullptr && update.context != nullptr);
    float dt = 2.0f;
    lux_script_value_slot update_slot{
        LUX_SCRIPT_VK_FLOAT,
        {},
        sizeof(float),
        lux::meta::ref_type_of_v<float>.hash,
        &dt
    };
    lux_script_call_frame update_frame{};
    update_frame.args = &update_slot;
    update_frame.arg_count = 1;
    update_frame.user_context = update.context;
    assert(update.invoke(&update_frame) == 0);
    assert(*static_cast<float*>(update.context) == 6.0f);

    auto prefix = exact;
    auto& prefix_native = std::get<lux::rdesc::NativeModuleScript>(prefix.body);
    prefix_native.functions[1].args.clear();
    const auto other_id = uuids::uuid::from_string(
        "97b2c826-ebfb-41a3-865c-a62b25b4d782"
    ).value();
    assert(!backend.createInstanceFromAsset(
        self, world, prefix, bytes, other_id, 1
    ));

    auto reordered = exact;
    auto& reordered_native =
        std::get<lux::rdesc::NativeModuleScript>(reordered.body);
    std::swap(
        reordered_native.functions[2].args[0],
        reordered_native.functions[2].args[1]
    );
    const auto third_id = uuids::uuid::from_string(
        "c3ab9539-d9a4-42e9-b3ee-f0393764c00f"
    ).value();
    assert(!backend.createInstanceFromAsset(
        self, world, reordered, bytes, third_id, 1
    ));

    auto kind_drift = exact;
    auto& kind_native =
        std::get<lux::rdesc::NativeModuleScript>(kind_drift.body);
    kind_native.functions[1].args[0].kind = LUX_SCRIPT_VK_DOUBLE;
    const auto fourth_id = uuids::uuid::from_string(
        "239d09a8-cee2-4a72-9640-af70bca83357"
    ).value();
    assert(!backend.createInstanceFromAsset(
        self, world, kind_drift, bytes, fourth_id, 1
    ));

    auto modified = description(pair_event, 99.0f);
    std::vector<std::byte> corrupt{std::byte{0x13}, std::byte{0x37}};
    auto cached = backend.createInstanceFromAsset(
        self,
        world,
        modified,
        corrupt,
        id,
        8
    );
    assert(cached);
    const auto cached_update = cached.entry(
        lux::ecs::ScriptEventRegistry::kOnUpdate
    );
    assert(cached_update.context != nullptr);
    assert(*static_cast<float*>(cached_update.context) == 4.0f);

    instance = {};
    cached = {};
    backend.resetSession();
    assert(!backend.createInstanceFromAsset(
        self, world, exact, corrupt, id, 9
    ));
    return 0;
}
