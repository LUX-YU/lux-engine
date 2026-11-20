#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/frame/FrameData.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string_view>

#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)
#error Frame columns must not import a native graphics API
#endif

using namespace lux::render;

namespace
{
    void check(bool value, int line)
    {
        if (value) return;
        std::fprintf(stderr, "Frame column check failed at %d\n", line);
        std::abort();
    }

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

    template <class T> T take(RenderResult<T> result)
    {
        CHECK(result);
        return std::move(*result);
    }

    struct alignas(64) Element final
    {
        std::uint64_t value = 7;
    };

    struct ResourceA final
    {
        using PerFrameData = std::tuple<Element, std::uint32_t>;
        struct Descriptor final { ResourceNameView type; };
        static inline const Descriptor descriptor{ResourceNameView{"test.frame.a"}};
    };

    struct ResourceB final
    {
        using PerFrameData = std::tuple<Element>;
        static inline const ResourceA::Descriptor descriptor{ResourceNameView{"test.frame.b"}};
    };

    struct ConflictingResource final
    {
        using PerFrameData = ResourceA::PerFrameData;
        static inline const auto descriptor = ResourceA::descriptor;
    };

    template <class R, class T> concept CanAppend = requires(FrameData& packet)
    {
        packet.appendData<R, T>(1);
    };

    static_assert(CanAppend<ResourceA, Element>);
    static_assert(!CanAppend<ResourceB, std::uint32_t>);

    struct ImmutableElement final
    {
        const std::uint64_t value = 0;
    };

    struct NonMovableElement final
    {
        std::uint64_t value = 0;
        NonMovableElement() noexcept = default;
        NonMovableElement(const NonMovableElement&) = delete;
        NonMovableElement(NonMovableElement&&) = delete;
        NonMovableElement& operator=(const NonMovableElement&) = default;
    };

    struct ResourceWithRestrictedElements final
    {
        using PerFrameData = std::tuple<ImmutableElement, NonMovableElement>;
    };

    template <class R, class T> concept CanAdd = requires(FrameData& packet, T&& value)
    {
        packet.addData<R>(std::forward<T>(value));
    };

    static_assert(CanAdd<ResourceA, const Element&> && CanAdd<ResourceA, Element>);
    static_assert(CanAppend<ResourceWithRestrictedElements, ImmutableElement>);
    static_assert(!CanAdd<ResourceWithRestrictedElements, ImmutableElement>);
    static_assert(std::is_trivially_copyable_v<NonMovableElement>);
    static_assert(!CanAppend<ResourceWithRestrictedElements, NonMovableElement>);
    static_assert(!CanAdd<ResourceWithRestrictedElements, NonMovableElement>);

    struct MoveOnlyConfig final
    {
        std::uint32_t value;
        explicit MoveOnlyConfig(std::uint32_t v) noexcept : value(v) {}
        MoveOnlyConfig(MoveOnlyConfig&&) noexcept = default;
        MoveOnlyConfig(const MoveOnlyConfig&) = delete;
    };

    template <class T> concept CanMakeConfig = requires(T&& value)
    {
        ConfigValue::make(std::forward<T>(value));
    };

    template <class T> concept CanBorrowConfig = requires(T&& owner)
    {
        std::forward<T>(owner).template borrow<std::uint32_t>();
        std::forward<T>(owner).data();
    };

    static_assert(CanMakeConfig<const std::uint32_t&> && CanMakeConfig<MoveOnlyConfig>);
    static_assert(!CanMakeConfig<MoveOnlyConfig&>);
    static_assert(CanBorrowConfig<const ConfigValue&> && !CanBorrowConfig<ConfigValue>);

    using Resources = std::tuple<ResourceA, ResourceB, ResourceA>;

    FrameLayout layout()
    {
        return take(makeFrameLayout(std::type_identity<Resources>{}, 3));
    }

    void columns()
    {
        const auto schema = layout();
        CHECK(schema.channels().size() == 3 && schema.revision() == 3);
        CHECK(!makeFrameLayout(std::type_identity<std::tuple<ResourceA, ConflictingResource>>{}, 3));
        CHECK(!FrameLayout::create(schema.channels(), 0));
        std::vector<FrameChannel> reordered(schema.channels().begin(), schema.channels().end());
        std::ranges::reverse(reordered);
        const auto reversed = take(FrameLayout::create(reordered, 4));
        CHECK(schema.hasSameChannels(reversed) && schema != reversed);
        auto packet = take(FrameData::create<Resources>(reversed));
        CHECK(packet.writable());
        CHECK(packet.getData<ResourceA, Element>().empty());
        CHECK(packet.appendData<ResourceA, Element>(0)->empty());
        CHECK(packet.reserve<ResourceA, Element>(2));
        auto first = take(packet.appendData<ResourceA, Element>(2));
        CHECK(first[0].value == 7 && first[1].value == 7);
        CHECK(reinterpret_cast<std::uintptr_t>(first.data()) % alignof(Element) == 0);
        first[0].value = 101;
        const auto* stable = first.data();
        CHECK(packet.appendData<ResourceB, Element>(4097));
        CHECK(packet.getData<ResourceA, Element>().data() == stable);
        CHECK(packet.getData<ResourceB, Element>().size() == 4097);
        CHECK(packet.addData<ResourceA>(std::uint32_t{25}));
        const Element copied{103};
        CHECK(packet.addData<ResourceA>(copied));
        CHECK(packet.getData<ResourceA, Element>().size() == 3);
        CHECK(packet.getData<ResourceA, Element>()[0].value == 101);
        CHECK(packet.getData<ResourceA, Element>()[2].value == 103);
        CHECK(packet.getData<ResourceA, std::uint32_t>()[0] == 25);
        const auto index = reversed.find(lux::cxx::make_basic_type_info<ResourceA>(),
            lux::cxx::make_basic_type_info<Element>());
        const auto capacity = packet.capacity(index);
        const auto* retained = packet.column(index).data;
        CHECK(capacity >= 4);
        CHECK(!packet.reserve<ResourceA, Element>(SIZE_MAX));
        CHECK(!packet.appendData<ResourceA, Element>(SIZE_MAX));
        CHECK(packet.capacity(index) == capacity && packet.column(index).count == 3);
        CHECK(packet.seal());
        CHECK(!packet.writable() && !packet.seal());
        CHECK(!packet.reserve<ResourceA, Element>(0));
        CHECK(!packet.appendData<ResourceA, Element>(0));
        CHECK(!packet.addData<ResourceA>(Element{}));
        // Another CPU slot can grow while the first is sealed; no global pool or GPU slot is involved.
        auto other = take(FrameData::create<Resources>(schema));
        CHECK(other.appendData<ResourceA, Element>(8193));
        CHECK(packet.column(index).data == retained && packet.column(index).count == 3);
        packet.release(); // This leaf fixture did not submit the borrow to a Server.
        packet.reset();
        CHECK(packet.column(index).count == 0 && packet.capacity(index) == capacity);
        CHECK(packet.column(index).data == retained);
        CHECK(packet.appendData<ResourceA, Element>(capacity));
        CHECK(packet.column(index).data == retained);
        auto moved = std::move(packet);
        CHECK(moved.column(index).count == capacity && !packet.writable());
        CHECK(!packet.addData<ResourceA>(Element{}));
        CHECK(!packet.appendData<ResourceA, Element>(1));
        moved.reset();

        auto single_schema = take(makeFrameLayout(std::type_identity<std::tuple<ResourceA>>{}, 1));
        auto single = take(FrameData::create<std::tuple<ResourceA>>(single_schema));
        CHECK(!single.appendData<ResourceB, Element>(1));
        CHECK(!FrameData::create<Resources>(single_schema));
        std::array<std::size_t, 1> wrong_capacities{1};
        CHECK(!FrameData::create<Resources>(schema, wrong_capacities));
        std::array<std::size_t, 3> capacities{8, 16, 32};
        auto reserved = take(FrameData::create<Resources>(schema, capacities));
        for (std::size_t i = 0; i < capacities.size(); ++i)
            CHECK(reserved.capacity(i) >= capacities[i] && reserved.column(i).count == 0);
        std::vector<FrameChannel> malformed(schema.channels().begin(), schema.channels().end());
        malformed[0].alignment = 3;
        CHECK(!FrameLayout::create(malformed, 3));
        malformed[0] = schema.channels()[0];
        malformed[0].size *= 2;
        const auto wrong_abi = take(FrameLayout::create(malformed, 3));
        CHECK(!FrameData::create<Resources>(wrong_abi));
        malformed.push_back(schema.channels()[0]);
        CHECK(!FrameLayout::create(malformed, 3));
        auto empty_schema = take(makeFrameLayout(std::type_identity<std::tuple<>>{}, 1));
        auto empty = take(FrameData::create<std::tuple<>>(empty_schema));
        CHECK(empty.seal());
        empty.release();
        empty.reset();
    }

    void ownershipViolation(std::string_view mode)
    {
        std::set_terminate([]() noexcept { std::_Exit(86); });
        auto packet = take(FrameData::create<Resources>(layout()));
        if (mode == "missing-read")
        {
            auto single_schema = take(makeFrameLayout(std::type_identity<std::tuple<ResourceA>>{}, 1));
            auto single = take(FrameData::create<std::tuple<ResourceA>>(single_schema));
            (void)single.getData<ResourceB, Element>();
            std::_Exit(1);
        }
        CHECK(packet.seal());
        if (mode == "reset") packet.reset();
        else if (mode == "move") { auto moved = std::move(packet); }
        else if (mode == "assign") packet = FrameData{};
        else if (mode != "destroy") std::_Exit(1);
    }
}

int main(int argc, char** argv)
{
    if (argc == 2)
    {
        ownershipViolation(argv[1]);
        return 1;
    }
    columns();
    std::puts("Frame columns: declaration identity, ABI, aligned growth, valid objects and retained capacity,");
    std::puts("overflow, sealed writes, two CPU slots and move ownership passed; no production frame routing claimed.");
}
