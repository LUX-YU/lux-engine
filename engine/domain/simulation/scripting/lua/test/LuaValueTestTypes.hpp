#pragma once
#include <lux/cxx/reflection/runtime/Marker.hpp>
#include <lux/engine/core/semantic/SemanticType.hpp>
#include <cstdint>
#include <array>
#include <utility>

namespace lux::simulation::script::test
{
    enum class LUX_META(luxlua::value) ValueMode : std::int32_t { WALK = 1, RUN = 3 };
    struct LUX_META(luxlua::value) ValueVelocity { float x; double y; };
    struct LUX_META(luxlua::value) ValuePose
    {
        LUX_META(luxlua::field, name = key) std::int32_t id;
        ValueVelocity velocity;
        ValueMode mode;
    };
    struct LUX_META(luxlua::value) ValueAngle { float radians; };
    struct LUX_META(luxlua::value) ValueConstRecord { const std::int32_t id; double weight; };
    struct LUX_META(luxlua::value) ValuePushOnly { float radians; };
    class LUX_META(luxlua::value) ValueToken
    {
    public:
        ValueToken() = delete;
        ValueToken(const ValueToken&) = delete;
        ValueToken(ValueToken&& other) noexcept : id_(std::exchange(other.id_, -1)) {}
        ~ValueToken() noexcept
        {
            if (id_ >= 0)
            {
                --live;
                released.at(release_count++) = id_;
            }
        }
        static ValueToken create(std::int32_t id) noexcept { return ValueToken{id}; }
        std::int32_t value() const noexcept { return id_; }
        inline static int live{};
        inline static std::size_t release_count{};
        inline static std::array<std::int32_t, 64> released{};
    private:
        explicit ValueToken(std::int32_t id) noexcept : id_(id) { ++live; }
        std::int32_t id_;
    };
}
namespace lux::semantic
{
    template <class T> struct TestLuaRecordTraits
    {
        inline static constexpr std::uint8_t AbiKind = static_cast<std::uint8_t>(EAbiKind::STRUCT_REF);
        inline static constexpr std::uint32_t Size = sizeof(T);
        inline static constexpr std::uint32_t Alignment = alignof(T);
    };
    template <> struct TypeTraits<lux::simulation::script::test::ValueVelocity>
        : TestLuaRecordTraits<lux::simulation::script::test::ValueVelocity>
    { inline static constexpr std::string_view CanonicalName = "lux.test.lua.velocity"; };
    template <> struct TypeTraits<lux::simulation::script::test::ValuePose>
        : TestLuaRecordTraits<lux::simulation::script::test::ValuePose>
    { inline static constexpr std::string_view CanonicalName = "lux.test.lua.pose"; };
    template <> struct TypeTraits<lux::simulation::script::test::ValueAngle>
        : TestLuaRecordTraits<lux::simulation::script::test::ValueAngle>
    { inline static constexpr std::string_view CanonicalName = "lux.test.lua.angle"; };
    template <> struct TypeTraits<lux::simulation::script::test::ValueMode>
        : TestLuaRecordTraits<lux::simulation::script::test::ValueMode>
    { inline static constexpr std::string_view CanonicalName = "lux.test.lua.mode"; };
    template <> struct TypeTraits<lux::simulation::script::test::ValueConstRecord>
        : TestLuaRecordTraits<lux::simulation::script::test::ValueConstRecord>
    { inline static constexpr std::string_view CanonicalName = "lux.test.lua.const_record"; };
    template <> struct TypeTraits<lux::simulation::script::test::ValuePushOnly>
        : TestLuaRecordTraits<lux::simulation::script::test::ValuePushOnly>
    { inline static constexpr std::string_view CanonicalName = "lux.test.lua.push_only"; };
    template <> struct TypeTraits<lux::simulation::script::test::ValueToken>
        : TestLuaRecordTraits<lux::simulation::script::test::ValueToken>
    { inline static constexpr std::string_view CanonicalName = "lux.test.lua.token"; };
}
