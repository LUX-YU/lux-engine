#include "FactoryAudit.hpp"
#include <lux/engine/render/resource/Storage.hpp>
#include <lux/engine/render/resource/Factory.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string_view>

using namespace lux::render;

namespace
{
    void require(bool condition)
    {
        if (!condition) std::abort();
    }
}

int main()
{
    constexpr StorageDescriptor original{StorageNameView("test.Storage")};
    require(bool(validateStorage(original)));
    require(bool(compatibleStorage(original, original)));
    auto changed = original;
    changed.type = StorageNameView("test.Other");
    require(!compatibleStorage(original, changed));
    changed = original;
    changed.type = {};
    require(!validateStorage(changed));
    changed = original;
    std::array binding{ShaderBinding{0, 0, EDescriptorKind::STORAGE_BUFFER, 1, 16}};
    changed.bindings = binding;
    require(bool(validateStorage(changed)) && !compatibleStorage(original, changed));
    binding[0].kind = static_cast<EDescriptorKind>(255);
    require(!validateStorage(changed));
    changed = original;
    changed.required_capabilities = 1;
    require(bool(validateStorage(changed)));
    require(!compatibleStorage(original, changed));
    changed.optional_capabilities = 1;
    require(!validateStorage(changed));
    changed = original;
    changed.optional_capabilities = 8;
    require(!validateStorage(changed));
    constexpr std::array expected_errors{
        std::string_view{"lux.render.resource.contract"},
        std::string_view{"lux.render.resource.mismatch"},
        std::string_view{"lux.render.resource.factory"},
        std::string_view{"lux.render.resource.capability"},
        std::string_view{"lux.render.resource.identity"},
        std::string_view{"lux.render.resource.capacity"},
        std::string_view{"lux.render.resource.cpu_stopped"},
        std::string_view{"lux.render.resource.cpu_schedule_failure"}
    };
    const auto errors = renderResourceErrorDescriptors();
    require(errors.size() == expected_errors.size());
    for (const auto name : expected_errors)
    {
        require(std::ranges::count_if(errors, [name](const auto& descriptor) noexcept {
            return descriptor.name == name && !descriptor.message.empty();
        }) == 1);
    }

    struct Value final { unsigned value; };
    struct Ops final { unsigned (*read)(const void*) noexcept; };
    const Ops ops{[](const void* value) noexcept { return static_cast<const Value*>(value)->value; }};
    auto factory = resource_detail::StorageFactoryAudit::make(
        &original, &ops,
        [](void*, StorageId, const void* config) noexcept -> RenderResult<NativeStorage> {
            return NativeStorage{new Value{*static_cast<const unsigned*>(config)}, [](void* value) noexcept {
                delete static_cast<Value*>(value);
            }};
        },
        [](const void* instance, const void* config) noexcept {
            return static_cast<const Value*>(instance)->value == *static_cast<const unsigned*>(config);
        }, {}, nullptr, storageCppType<unsigned>(), storageCppType<Ops>()
    );
    require(bool(validateStorageFactory(original, factory)));
    auto wrong_factory = factory;
    wrong_factory.description = nullptr;
    require(!validateStorageFactory(original, wrong_factory));
    const unsigned config = 73, other = 19;
    auto instance = factory.create(nullptr, StorageId{1, 1}, &config);
    require(bool(instance));
    require(ops.read(instance->get()) == config);
    require(factory.compatible_config(instance->get(), &config));
    require(!factory.compatible_config(instance->get(), &other));
    factory.ops = nullptr;
    require(!validateStorageFactory(original, factory));
}
