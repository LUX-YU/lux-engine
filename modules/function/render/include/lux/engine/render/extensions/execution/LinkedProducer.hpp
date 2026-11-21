#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/extensions/execution/ProducerWorkspace.hpp>

namespace lux::render
{
    struct TargetDesc;

    class InstanceInstantiationContext
    {
    public:
        virtual ~InstanceInstantiationContext() noexcept = default;
        [[nodiscard]] virtual const TargetDesc& target() const noexcept = 0;
    };

    class LinkedProducer final
    {
    public:
        [[nodiscard]] RenderResult<ProducerWorkspace> instantiate(InstanceInstantiationContext& instance) const & noexcept
        {
            return instantiate_(inputs_.data(), instance);
        }

        RenderResult<ProducerWorkspace> instantiate(InstanceInstantiationContext&) const && = delete;

    private:
        friend class ProducerFactory;
        using Instantiate = RenderResult<ProducerWorkspace> (*)(const void*, InstanceInstantiationContext&) noexcept;

        LinkedProducer(ConfigValue inputs, Instantiate instantiate) noexcept
            : inputs_(std::move(inputs)), instantiate_(instantiate)
        {
        }

        ConfigValue inputs_;
        Instantiate instantiate_;
    };
}
