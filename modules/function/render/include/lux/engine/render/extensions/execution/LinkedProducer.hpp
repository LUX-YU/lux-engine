#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/extensions/execution/ProducerWorkspace.hpp>

namespace lux::render
{
    struct TargetDesc;

    class LinkedProducer final
    {
    public:
        [[nodiscard]] RenderResult<ProducerWorkspace> instantiate(const TargetDesc& target) const & noexcept
        {
            return instantiate_(inputs_.data(), target);
        }

        RenderResult<ProducerWorkspace> instantiate(const TargetDesc&) const && = delete;

        [[nodiscard]] const ConfigValue& inputs() const & noexcept { return inputs_; }
        const ConfigValue& inputs() const && = delete;
        [[nodiscard]] cxx::basic_type_info mappingType() const noexcept
        {
            return mapping_type_ ? *mapping_type_ : cxx::make_basic_type_info<void>();
        }

    private:
        friend class ProducerFactory;
        using Instantiate = RenderResult<ProducerWorkspace> (*)(const void*, const TargetDesc&) noexcept;

        LinkedProducer(ConfigValue inputs, Instantiate instantiate, const cxx::basic_type_info* mapping) noexcept
            : inputs_(std::move(inputs)), instantiate_(instantiate), mapping_type_(mapping)
        {
        }

        ConfigValue inputs_;
        Instantiate instantiate_;
        const cxx::basic_type_info* mapping_type_;
    };
}
