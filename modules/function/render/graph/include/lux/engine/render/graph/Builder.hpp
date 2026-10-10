#pragma once

#include <lux/engine/render/graph/Schema.hpp>
#include <utility>

namespace lux::render
{
    struct ComputeShaderReference
    {
        ShaderReference shader;
    };

    struct GraphicsShaderReference
    {
        ShaderReference shader;
    };

    struct AuthoringFallback
    {
        std::variant<GraphTexture, GraphBuffer> resource;
        VGraphProducer producer{ImportedProducer{}};
    };

    class RenderGraphBuilder
    {
    public:
        RenderGraphBuilder() noexcept;
        RenderGraphBuilder(const RenderGraphBuilder&) = delete;
        RenderGraphBuilder& operator=(const RenderGraphBuilder&) = delete;
        RenderGraphBuilder(RenderGraphBuilder&& other) noexcept;
        RenderGraphBuilder& operator=(RenderGraphBuilder&& other) noexcept;

        [[nodiscard]] GraphTexture texture(TextureDesc description, std::string_view name = {}) noexcept;

        [[nodiscard]] GraphBuffer buffer(BufferDesc description, std::string_view name = {}) noexcept;

        [[nodiscard]] GraphTexture importTexture(
            std::string_view name,
            TextureDesc description,
            EPersistentScope scope,
            GraphImportContract contract = {}
        ) noexcept;

        [[nodiscard]] GraphBuffer importBuffer(
            std::string_view name,
            BufferDesc description,
            EPersistentScope scope,
            GraphImportContract contract = {}
        ) noexcept;

        [[nodiscard]] GraphTexture importTarget(
            std::string_view name,
            TextureDesc description,
            RenderTargetSemanticId target,
            GraphImportContract contract = {}
        ) noexcept;

        template <GraphPassParameters T>
        [[nodiscard]] RenderResult<GraphPassId> addPass(
            std::string_view name,
            ShaderReference shader_reference,
            EPassKind kind,
            EExecutionScope scope,
            const T& parameters
        ) noexcept
        {
            return addCapturedPass(
                name,
                shader_reference,
                kind,
                scope,
                PassSchema<T>::contract(),
                PassSchema<T>::capture(parameters)
            );
        }

        template <GraphPassParameters T>
        [[nodiscard]] RenderResult<GraphPassId> compute(
            std::string_view name,
            ComputeShaderReference shader,
            EExecutionScope scope,
            const T& parameters
        ) noexcept
        {
            return addPass(name, shader.shader, EPassKind::COMPUTE, scope, parameters);
        }

        template <GraphPassParameters T>
        [[nodiscard]] RenderResult<GraphPassId> graphics(
            std::string_view name,
            GraphicsShaderReference shader,
            EExecutionScope scope,
            const T& parameters
        ) noexcept
        {
            return addPass(name, shader.shader, EPassKind::GRAPHICS, scope, parameters);
        }

        template <GraphPassParameters T>
        [[nodiscard]] RenderResult<GraphPassId> transfer(
            std::string_view name,
            EExecutionScope scope,
            const T& parameters
        ) noexcept
        {
            return addPass(name, {}, EPassKind::TRANSFER, scope, parameters);
        }

        template <GraphPassParameters T>
        [[nodiscard]] RenderResult<GraphPassId> readback(
            std::string_view name,
            EExecutionScope scope,
            const T& parameters
        ) noexcept
        {
            return addPass(name, {}, EPassKind::HOST_READBACK, scope, parameters);
        }

        [[nodiscard]] RenderResult<void> after(PassKey before, PassKey after) noexcept;
        [[nodiscard]] RenderResult<void> source(
            PassKey pass,
            std::string_view field,
            VGraphProducer producer,
            std::optional<AuthoringFallback> fallback = {},
            std::uint32_t array_element = 0
        ) noexcept;
        [[nodiscard]] RenderResult<void> condition(PassKey pass, GraphResourceKey group) noexcept;
        [[nodiscard]] RenderResult<void> invocationInputs(PassKey pass, std::uint32_t mask) noexcept;
        [[nodiscard]] RenderResult<void> provide(
            GraphResourceKey semantic,
            GraphTexture resource,
            PassKey pass
        ) noexcept;
        [[nodiscard]] RenderResult<void> provide(
            GraphResourceKey semantic,
            GraphBuffer resource,
            PassKey pass
        ) noexcept;
        [[nodiscard]] RenderResult<void> localRead(PassKey pass, std::string_view field) noexcept;
        [[nodiscard]] RenderResult<void> exportTexture(
            GraphTexture resource,
            VGraphProducer producer,
            EGraphOutput kind = EGraphOutput::EXPORT,
            ImageRange range = {EAspect::COLOR, 0, kRemainingSubresources, 0, kRemainingSubresources},
            GraphResourceKey semantic = {}
        ) noexcept;
        [[nodiscard]] RenderResult<void> exportBuffer(
            GraphBuffer resource,
            VGraphProducer producer,
            EGraphOutput kind = EGraphOutput::EXPORT,
            BufferRange range = {},
            GraphResourceKey semantic = {}
        ) noexcept;

        // Consumes this declaration scope; the emptied Builder receives a fresh scope.
        [[nodiscard]] RenderResult<RenderGraphDefinition> finish() && noexcept;

    private:
        [[nodiscard]] RenderResult<GraphPassId> addCapturedPass(
            std::string_view name,
            ShaderReference shader_reference,
            EPassKind kind,
            EExecutionScope scope,
            rdesc::PassShaderContract contract,
            detail::CapturedParameters captured
        ) noexcept;

        void swap(RenderGraphBuilder& other) noexcept;

        std::uint64_t scope_;
        std::optional<RenderError> error_;
        std::vector<GraphResource> resources_;
        std::vector<GraphPass> passes_;
        std::vector<std::pair<PassKey, PassKey>> order_;
        std::vector<GraphOutput> outputs_;
        std::vector<GraphProvider> providers_;
    };
} // namespace lux::render
