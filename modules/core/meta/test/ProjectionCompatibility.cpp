#include "ProjectionTypes.hpp"
#include "ProjectionTypes.serialize.hpp"
#include "ProjectionTypes.type_static_info.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace
{
    using lux::meta::test::EMode;
    using lux::meta::test::Position;
    using lux::meta::test::Settings;
    namespace ser = lux::cxx::ser;

    template <class A, class B> constexpr bool samePointer(A lhs, B rhs) noexcept
    {
        if constexpr (std::is_same_v<A, B>)
        {
            return lhs == rhs;
        }
        return false;
    }

    template <class T, class F> void compareMembers(T& value, F field) noexcept
    {
        bool matched{};
        std::apply(
            [&](const auto&... engine_field) noexcept
            {
                const auto compare = [&](const auto& engine) noexcept
                {
                    if constexpr (std::is_same_v<decltype(field.pointer), std::remove_cv_t<decltype(engine.pointer)>>)
                    {
                        if (field.pointer == engine.pointer)
                        {
                            assert(&(value.*field.pointer) == &(value.*engine.pointer));
                            matched = true;
                        }
                    }
                };
                (compare(engine_field), ...);
            },
            lux::meta::TTypeStaticInfo<T>::fields
        );
        const bool excluded_from_engine = field.name == "archive_only" || field.name == "cached";
        assert(matched != excluded_from_engine);
    }

    void plainProjection() noexcept
    {
        constexpr auto fields = lux::meta::TTypeStaticInfo<Position>::fields;
        static_assert(std::tuple_size_v<decltype(fields)> == 2);
        static_assert(ser::field_count_v<Position> == 2);
        static_assert(std::get<0>(fields).pointer == &Position::x);
        static_assert(std::get<1>(fields).pointer == &Position::y);
        Position value{1, 2};
        std::size_t index{};
        ser::meta_info<Position>::for_each_field(
            [&](const auto& field) noexcept
            {
                assert(field.name == (index == 0 ? "x" : "y"));
                assert(!field.options.skip);
                compareMembers(value, field);
                value.*field.pointer += 10;
                ++index;
            }
        );
        assert(index == 2 && value.x == 11 && value.y == 12);
    }

    void policyProjection() noexcept
    {
        constexpr auto fields = lux::meta::TTypeStaticInfo<Settings>::fields;
        static_assert(std::tuple_size_v<decltype(fields)> == 7);
        static_assert(ser::field_count_v<Settings> == 9);
        static_assert(std::get<0>(fields).pointer == &Settings::identity);
        static_assert(std::get<1>(fields).pointer == &Settings::label);
        static_assert(std::get<2>(fields).pointer == &Settings::mode);
        static_assert(std::get<3>(fields).pointer == &Settings::modes);
        static_assert(std::get<4>(fields).pointer == &Settings::dimensions);
        static_assert(std::get<5>(fields).pointer == &Settings::position);
        static_assert(std::get<6>(fields).pointer == &Settings::editor_only);
        static_assert(std::get<1>(fields).name == "label");
        constexpr std::array<std::string_view, 9> names{
            "identity",
            "display_name",
            "mode",
            "modes",
            "dimensions",
            "position",
            "editor_only",
            "archive_only",
            "cached"
        };
        Settings value;
        std::size_t index{};
        std::size_t serialized{};
        ser::meta_info<Settings>::for_each_field(
            [&](const auto& field) noexcept
            {
                using Member = typename std::remove_cvref_t<decltype(field)>::member_type;
                assert(field.name == names[index++]);
                compareMembers(value, field);
                assert(field.options.skip == (field.name == "editor_only" || field.name == "cached"));
                assert(field.options.required == (field.name == "display_name"));
                serialized += !field.options.skip;
                if constexpr (std::is_same_v<Member, std::string>)
                {
                    assert(samePointer(field.pointer, &Settings::label));
                    value.*field.pointer = "typed name";
                }
                else if constexpr (std::is_same_v<Member, std::vector<EMode>>)
                {
                    assert(samePointer(field.pointer, &Settings::modes));
                    (value.*field.pointer).push_back(EMode::ACTIVE);
                }
                else if constexpr (std::is_same_v<Member, std::array<float, 3>>)
                {
                    assert(samePointer(field.pointer, &Settings::dimensions));
                    value.*field.pointer = {4, 5, 6};
                }
                else if constexpr (std::is_same_v<Member, Position>)
                {
                    assert(samePointer(field.pointer, &Settings::position));
                    value.*field.pointer = {7, 8};
                }
            }
        );
        assert(index == names.size() && serialized == 7);
        assert(value.label == "typed name" && value.modes == std::vector{EMode::ACTIVE});
        assert((value.dimensions == std::array<float, 3>{4, 5, 6}));
        assert(value.position.x == 7 && value.position.y == 8);
        // Equal effective counts do not imply equal schemas: one projects editor_only,
        // the other archive_only. No offset-based access or runtime registry is needed.
    }

    void enumProjection() noexcept
    {
        static_assert(!lux::meta::HasTypeStaticInfo<EMode>);
        static_assert(ser::ReflectedEnum<EMode>);
        static_assert(ser::enum_meta<EMode>::count == 2);
        static_assert(ser::enum_to_string(EMode::IDLE) == "IDLE");
        static_assert(ser::enum_to_string(EMode::ACTIVE) == "ACTIVE");
        EMode value = EMode::IDLE;
        assert(ser::enum_from_string("ACTIVE", value) && value == EMode::ACTIVE);
        assert(!ser::enum_from_string("unknown", value) && value == EMode::ACTIVE);
        assert(ser::enum_to_string(static_cast<EMode>(255)).empty());
    }
} // namespace

int main()
{
    plainProjection();
    policyProjection();
    enumProjection();
    std::puts("PASS generated projections: order/member/skip/name/enum/container; distinct domain policies retained");
}
