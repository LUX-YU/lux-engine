#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/effect/EffectCapability.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lux::render
{
    class EffectCompileContext;
    // Owned by one logical Effect node. Device builds read immutable configuration.
    // Commands and frozen parameter bindings refer to the state subobject on the server owner.
    class EffectImplementation
    {
    public:
        struct State final
        {
            void* object = nullptr;
            cxx::basic_type_info type;
        };

        virtual ~EffectImplementation() noexcept = default;
        EffectImplementation(const EffectImplementation&) = delete;
        EffectImplementation& operator=(const EffectImplementation&) = delete;
        [[nodiscard]] virtual RenderResult<void> onBuild(EffectCompileContext&) const noexcept = 0;
        [[nodiscard]] State state() const noexcept { return state_; }
        [[nodiscard]] std::size_t configurationBytes() const noexcept { return configuration_bytes_; }

    protected:
        explicit EffectImplementation(std::size_t configuration_bytes = 0) noexcept
            : configuration_bytes_(configuration_bytes) {}

        // Bind the implementation's own stable command state during construction.
        template <class T> void bindState(T& value) noexcept
        {
            state_ = {std::addressof(value), cxx::make_basic_type_info<T>()};
        }

    private:
        std::size_t configuration_bytes_;
        State state_;
    };

    using EffectCreateResult = RenderResult<std::unique_ptr<EffectImplementation>>;

    struct ImplementationEntry final
    {
        using Create = EffectCreateResult (*)(graph::NodeId, std::string_view arguments) noexcept;

        graph::NodeTypeId capability;
        std::uint32_t capability_version = 1;
        ImplementationSelection identity;
        std::string display_name;
        std::vector<graph::PinSemanticId> supported_outputs;
        Create create = nullptr;
    };
}
