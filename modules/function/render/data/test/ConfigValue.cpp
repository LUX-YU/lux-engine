#include "ConfigFixture.hpp"
#include <lux/engine/render/data/Payload.hpp>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <cstdio>
#include <windows.h>

using namespace lux::render;

namespace
{
    void check(bool condition) { if (!condition) std::abort(); }
    template <class T> T take(RenderResult<T> value)
    {
        check(bool(value));
        return std::move(*value);
    }

    struct AssetConfig final { RenderPayload asset; };
    struct Unaccounted final { std::vector<unsigned> values; };
    struct Undercounted final { std::uint64_t value; };

    template <class T> concept CanMakeConfig = requires(T value) { ConfigValue::make(std::move(value)); };
}

namespace lux::render
{
    template <> struct ConfigValueTraits<AssetConfig>
    {
        static RenderResult<std::size_t> retainedBytes(const AssetConfig& value) noexcept
        {
            const auto size = cxx::checkedAdd(sizeof(value), value.asset.retainedBytes());
            if (!size) return cxx::unexpected(RenderError{kInvalidConfig});
            return *size;
        }
    };

    template <> struct ConfigValueTraits<Undercounted>
    {
        static RenderResult<std::size_t> retainedBytes(const Undercounted&) noexcept { return 1; }
    };
}

int main()
{
    static_assert(!CanMakeConfig<Unaccounted>);
    static_assert(std::is_same_v<decltype(std::declval<const ConfigValue&>().borrow<unsigned>()), const unsigned*>);
    check(!ConfigValue::make(Undercounted{}));
    ConfigValue empty;
    check(!empty && !empty.type().isValid() && empty.retainedBytes() == 0);
    auto scalar = take(ConfigValue::make(17u));
    check(scalar == take(ConfigValue::make(17u)));
    check(scalar != take(ConfigValue::make(18u)) && scalar != take(ConfigValue::make(17.0f)));
    check(empty == ConfigValue{} && scalar != empty);
    check(*scalar.borrow<unsigned>() == 17 && scalar.borrow<float>() == nullptr);
    auto copied = scalar;
    check(copied.data() == scalar.data());
    auto moved = std::move(scalar);
    check(!scalar && !scalar.type().isValid() && scalar.retainedBytes() == 0);
    check(moved.data() == copied.data());

    unsigned destroyed = 0;
    auto source = std::shared_ptr<const std::vector<std::byte>>(
        new std::vector<std::byte>(4096, std::byte{0x71}), [&](const auto* value) { delete value; ++destroyed; }
    );
    const auto* original = source->data();
    auto asset = take(ConfigValue::make(AssetConfig{take(RenderPayload::fromVector(source, 31, 2))}));
    check(asset.retainedBytes() == sizeof(AssetConfig) + source->capacity());
    source.reset();
    check(destroyed == 0 && asset.borrow<AssetConfig>()->asset.bytes().data() == original + 31);
    auto retained = asset;
    check(retained == asset);
    check(retained != take(ConfigValue::make(AssetConfig{asset.borrow<AssetConfig>()->asset})));
    asset = {};
    check(destroyed == 0);
    retained = {};
    check(destroyed == 1);

    auto module = LoadLibraryA("render_config_plugin.dll");
    check(module != nullptr);
    using Make = bool (*)(bool*, unsigned*, ConfigValue*) noexcept;
    const auto make = reinterpret_cast<Make>(GetProcAddress(module, "makeForeignConfig"));
    check(make != nullptr);
    bool pin_alive = true;
    unsigned foreign_destroyed = 0;
    auto pin = std::shared_ptr<const void>(module, [&](const void* value) noexcept {
        check(foreign_destroyed == 1);
        FreeLibrary(static_cast<HMODULE>(const_cast<void*>(value)));
        pin_alive = false;
    });
    ConfigValue foreign;
    check(make( &pin_alive, &foreign_destroyed, &foreign));
    const auto* object = foreign.borrow<config_test::ForeignConfig>();
    check(object && object->bytes.size() == 3 && object->bytes.capacity() >= 8192);
    check(reinterpret_cast<std::uintptr_t>(object) % alignof(config_test::ForeignConfig) == 0);
    check(foreign.retainedBytes() == sizeof(*object) + object->bytes.capacity());
    auto late = foreign;
    unsigned local_destroyed = 0;
    bool local_alive = true;
    {
        auto local = take(ConfigValue::make(config_test::ForeignConfig{
            std::vector<std::byte>(3, std::byte{0x43}), local_alive, local_destroyed
        }));
        check(local.data() != foreign.data() && local == foreign && foreign == local);
        check(pin_alive && foreign_destroyed == 0);
    }
    check(local_destroyed == 1);
    foreign = take(ConfigValue::make(91u));
    check(pin_alive && foreign_destroyed == 0 && late.data() == object);
    late = {};
    check(pin_alive && foreign_destroyed == 1);
    pin.reset(); // Host unloads only after the final foreign object destructor returned.
    check(!pin_alive);
    std::printf("ConfigValue sizeof=%zu align=%zu: retained backing, cold type and foreign lifetime PASS\n",
        sizeof(ConfigValue), alignof(ConfigValue));
}
