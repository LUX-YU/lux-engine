#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/extensions/execution/InstanceLink.hpp>
#include <vector>

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

    template <class T>
    concept InstanceDataProducer = std::derived_from<T, InstanceData> && requires(
        const typename T::Configuration& config, typename T::ResolvedInputs inputs,
        InstanceLinkContext& link, InstanceInstantiationContext& instance
    )
    {
        { T::resolve(config, link) } noexcept -> std::same_as<RenderResult<typename T::ResolvedInputs>>;
        { T::instantiate(std::as_const(inputs), instance) } noexcept -> std::same_as<InstanceDataResult>;
        { ConfigValue::make(std::move(inputs)) } noexcept -> std::same_as<RenderResult<ConfigValue>>;
    };

    class InstanceDataFactory final
    {
    public:
        template <InstanceDataProducer T>
        [[nodiscard]] static constexpr InstanceDataFactory make() noexcept
        {
            return InstanceDataFactory{[](const ConfigValue& config, InstanceLinkContext& link) noexcept
                -> RenderResult<InstanceDataPrototype>
            {
                const auto* typed = config.borrow<typename T::Configuration>();
                if (!typed) return cxx::unexpected(RenderError{kInvalidConfig});
                auto resolved = T::resolve(*typed, link);
                if (!resolved) return cxx::unexpected(resolved.error());
                auto inputs = ConfigValue::make(std::move(*resolved));
                if (!inputs) return cxx::unexpected(inputs.error());
                return InstanceDataPrototype{std::move(*inputs),
                    [](const void* data, InstanceInstantiationContext& instance) noexcept -> InstanceDataResult
                    {
                        return T::instantiate(*static_cast<const typename T::ResolvedInputs*>(data), instance);
                    }
                };
            }};
        }

        [[nodiscard]] RenderResult<InstanceDataPrototype>
        link(const ConfigValue& config, InstanceLinkContext& context) const noexcept
        {
            return link_(config, context);
        }

    private:
        using Link = RenderResult<InstanceDataPrototype> (*)(const ConfigValue&, InstanceLinkContext&) noexcept;
        explicit constexpr InstanceDataFactory(Link link) noexcept : link_(link) {}
        Link link_;
    };

    struct GraphPreparation final
    {
        InstanceDataFactory factory;
        ConfigValue configuration;
        std::vector<GraphCpuDataId> inputs;
        std::vector<GraphCpuDataId> outputs;
        std::vector<GraphPassId> inline_blocks;
        // A live preparation retains all of its GPU passes. Inline block passes are included automatically.
        std::vector<GraphPassId> passes{};
        // Each listed buffer is published once, through its linked writer or sizer.
        std::vector<GraphResourceId> gpu_buffers{};
        std::vector<GraphStorageSnapshotId> storage_inputs{};
        // Subset of storage_inputs that drives the runtime Raster program selection.
        std::vector<GraphStorageSnapshotId> program_storage_inputs{};
    };
} // namespace lux::render
