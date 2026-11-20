#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/extensions/execution/InstanceData.hpp>
#include <memory>
#include <utility>

namespace lux::render
{
    class InstanceInstantiationContext
    {
    public:
        virtual ~InstanceInstantiationContext() noexcept = default;
        [[nodiscard]] virtual const TargetDesc& target() const noexcept = 0;
    };

    using InstanceDataResult = RenderResult<std::unique_ptr<InstanceData>>;

    class InstanceDataFactory;

    class InstanceDataPrototype final
    {
    public:
        [[nodiscard]] InstanceDataResult instantiate(InstanceInstantiationContext& instance) const noexcept
        {
            return instantiate_(inputs_.data(), instance);
        }

    private:
        friend class InstanceDataFactory;
        using Instantiate = InstanceDataResult (*)(const void*, InstanceInstantiationContext&) noexcept;

        InstanceDataPrototype(ConfigValue inputs, Instantiate instantiate) noexcept
            : inputs_(std::move(inputs)), instantiate_(instantiate)
        {
        }

        ConfigValue inputs_;
        Instantiate instantiate_;
    };
} // namespace lux::render
