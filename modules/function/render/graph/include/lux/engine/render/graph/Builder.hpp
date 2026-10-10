#pragma once

#include <lux/engine/render/graph/Schema.hpp>
#include <utility>

namespace lux::render
{
    class RenderGraphBuilder
    {
    public:
        [[nodiscard]] GraphTexture texture(TextureDesc description, std::string_view name = {}) noexcept;

        [[nodiscard]] GraphBuffer buffer(BufferDesc description, std::string_view name = {}) noexcept;

        [[nodiscard]] GraphTexture importTexture(
            std::string_view name,
            TextureDesc description,
            EPersistentScope scope
        ) noexcept;

        [[nodiscard]] GraphBuffer importBuffer(
            std::string_view name,
            BufferDesc description,
            EPersistentScope scope
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
            for (const auto& field : PassSchema<T>::contract().resources)
            {
                const auto role = field.role;
                const bool is_attachment =
                    role == rdesc::EPassFieldRole::COLOR_ATTACHMENT || role == rdesc::EPassFieldRole::DEPTH_STENCIL ||
                    role == rdesc::EPassFieldRole::RESOLVE || role == rdesc::EPassFieldRole::INPUT_ATTACHMENT;
                const bool is_transfer = role == rdesc::EPassFieldRole::TRANSFER_SOURCE ||
                                         role == rdesc::EPassFieldRole::TRANSFER_DESTINATION;
                const bool is_invalid_attachment = is_attachment && kind != EPassKind::GRAPHICS;
                const auto stage_mask = kind == EPassKind::COMPUTE ? 4u : 3u;
                const bool is_shader_field = role <= rdesc::EPassFieldRole::READ_WRITE_STORAGE ||
                                             role == rdesc::EPassFieldRole::INPUT_ATTACHMENT;
                const bool is_invalid_stage = is_shader_field && (field.stages & stage_mask) == 0;
                const bool is_invalid_transfer =
                    is_transfer && kind != EPassKind::TRANSFER && kind != EPassKind::HOST_READBACK;
                const bool is_invalid_native_only =
                    !is_transfer && (kind == EPassKind::TRANSFER || kind == EPassKind::HOST_READBACK);
                if (is_invalid_stage || is_invalid_attachment || is_invalid_transfer || is_invalid_native_only)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidUse, {passes_.size() + 1, 0}});
                }
            }
            auto captured = PassSchema<T>::capture(parameters);
            if (captured.error)
            {
                return cxx::unexpected(*captured.error);
            }
            const auto key = passKey(name);
            const auto shader = shaderKey(shader_reference);
            const bool is_invalid_key = !key.isValid();
            const bool needs_shader = kind == EPassKind::COMPUTE || kind == EPassKind::GRAPHICS;
            const bool is_invalid_shader = needs_shader && !shader.isValid();
            if (is_invalid_key || is_invalid_shader)
            {
                return cxx::unexpected(RenderError{kGraphInvalidUse, {passes_.size() + 1, 0}});
            }
            for (const auto& pass : passes_)
            {
                if (pass.key == key)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidUse, {passes_.size() + 1, key.value()}});
                }
            }
            GraphPass pass;
            pass.uses = std::move(captured.uses);
            pass.bindings = std::move(captured.bindings);
            pass.initial_scalars = std::move(captured.scalars);
            pass.canonical_name = name;
            pass.shader_name = shader_reference.canonicalName();
            pass.key = key;
            pass.shader = shader;
            pass.kind = kind;
            pass.scope = scope;
            const auto contract = PassSchema<T>::contract();
            pass.schema_name = contract.canonical_name;
            pass.shader_declarations = contract.declarations;
            for (const auto& field : contract.scalars)
            {
                pass.scalar_fields.push_back(
                    {std::string(field.path),
                     field.kind,
                     field.offset,
                     field.size,
                     field.array_stride,
                     field.array_count,
                     field.owner,
                     field.frequency,
                     field.stages}
                );
            }
            passes_.push_back(std::move(pass));
            return GraphPassId{static_cast<std::uint32_t>(passes_.size())};
        }

        [[nodiscard]] RenderResult<RenderGraphDefinition> finish() && noexcept
        {
            if (error_)
            {
                return cxx::unexpected(*error_);
            }
            return RenderGraphDefinition::create(std::move(resources_), std::move(passes_));
        }

    private:
        std::optional<RenderError> error_;
        std::vector<GraphResource> resources_;
        std::vector<GraphPass> passes_;
    };
} // namespace lux::render
