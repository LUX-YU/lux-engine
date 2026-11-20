#pragma once

#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/extensions/graph/ConstantBlock.hpp>
#include <cstring>

namespace lux::render
{
    // An Effect node owns this CPU value. Frame selection copies it by revision on the Server owner.
    template <ShaderConstantBlock T> class EffectUniformBlock final
    {
    public:
        explicit EffectUniformBlock(const T& value) noexcept : value_(value) {}

        [[nodiscard]] const T& value() const & noexcept { return value_; }
        const T& value() const && = delete;
        [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

        [[nodiscard]] RenderResult<void> set(const T& value) noexcept
        {
            if (revision_ == UINT64_MAX) return cxx::unexpected(RenderError{kRuntimeCapacity});
            std::memmove(&value_, &value, sizeof(T));
            ++revision_;
            return {};
        }

    private:
        T value_;
        std::uint64_t revision_ = 1;
    };

    namespace extension_detail
    {
        struct UniformSnapshot final
        {
            std::span<const std::byte> bytes;
            std::uint64_t revision;
        };

        struct EffectUniformSource final
        {
            cxx::basic_type_info state_type;
            ConstantBlockContract block;
            UniformSnapshot (*select)(const void*) noexcept;
        };
    }
} // namespace lux::render
