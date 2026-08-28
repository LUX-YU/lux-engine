#include <lux/engine/meta/RuntimeObject.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <utility>

namespace
{
    struct alignas(64) OverAlignedValue final
    {
        std::array<std::uint64_t, 8U> values{};
    };

    struct alignas(64) TrackedValue final
    {
        inline static std::size_t constructions{};
        inline static std::size_t destructions{};

        TrackedValue()
        {
            ++constructions;
        }

        TrackedValue(const TrackedValue&)
        {
            ++constructions;
        }

        ~TrackedValue()
        {
            ++destructions;
        }
    };

    [[nodiscard]] lux::meta::RefClass trackedMetadata()
    {
        lux::meta::RefClass metadata;
        metadata.type = lux::meta::ref_type_of_v<TrackedValue>;
        metadata.type.ptr = &metadata;
        metadata.construct = [](void* storage) { new (storage) TrackedValue(); };
        metadata.destruct = [](void* storage) { static_cast<TrackedValue*>(storage)->~TrackedValue(); };
        metadata.copy = [](void* destination, const void* source)
        {
            *static_cast<TrackedValue*>(destination) = *static_cast<const TrackedValue*>(source);
        };
        metadata.copy_construct = [](void* destination, const void* source)
        {
            new (destination) TrackedValue(*static_cast<const TrackedValue*>(source));
        };
        return metadata;
    }
}

int main()
{
    using lux::meta::ERuntimeObjectError;
    using lux::meta::RuntimeObject;

    auto over_aligned = RuntimeObject::defaultOf(lux::meta::builtin_ref_type_ptr<OverAlignedValue>());
    assert(over_aligned);
    assert(reinterpret_cast<std::uintptr_t>(over_aligned.data()) % alignof(OverAlignedValue) == 0U);
    over_aligned.get<OverAlignedValue>().values[3U] = 91U;

    RuntimeObject cloned;
    assert(over_aligned.copyTo(cloned));
    assert(cloned.get<OverAlignedValue>().values[3U] == 91U);
    assert(reinterpret_cast<std::uintptr_t>(cloned.data()) % alignof(OverAlignedValue) == 0U);

    auto metadata = trackedMetadata();
    metadata.type.ptr = &metadata;
    {
        auto tracked = RuntimeObject::create(&metadata);
        assert(tracked);
        assert(reinterpret_cast<std::uintptr_t>(tracked->data()) % alignof(TrackedValue) == 0U);
        RuntimeObject tracked_copy;
        assert(tracked->copyTo(tracked_copy));
    }
    assert(TrackedValue::constructions == 2U);
    assert(TrackedValue::destructions == 2U);

    metadata.construct = nullptr;
    const auto unavailable = RuntimeObject::create(&metadata);
    assert(!unavailable);
    assert(unavailable.error() == ERuntimeObjectError::CONSTRUCTION_UNAVAILABLE);
    return 0;
}
