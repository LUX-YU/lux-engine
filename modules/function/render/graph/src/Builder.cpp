#include <lux/engine/render/graph/Builder.hpp>

#include <atomic>
#include <exception>
#include <limits>

namespace lux::render
{
    namespace
    {
        std::uint64_t nextAuthoringScope() noexcept
        {
            // One cold scalar counter per linked Graph module, not a registry or per-resource owner.
            // Handles belong to this module's authoring domain; no serialization/plugin ABI is implied.
            static std::atomic<std::uint64_t> next{1};
            const auto value = next.fetch_add(1, std::memory_order_relaxed);
            if (value == 0 || value == std::numeric_limits<std::uint64_t>::max())
            {
                std::terminate();
            }
            return value;
        }
    } // namespace

    RenderGraphBuilder::RenderGraphBuilder() noexcept : scope_(nextAuthoringScope()) {}

    RenderGraphBuilder::RenderGraphBuilder(RenderGraphBuilder&& other) noexcept : RenderGraphBuilder()
    {
        swap(other);
    }

    RenderGraphBuilder& RenderGraphBuilder::operator=(RenderGraphBuilder&& other) noexcept
    {
        if (this != &other)
        {
            RenderGraphBuilder candidate(std::move(other));
            swap(candidate);
        }
        return *this;
    }

    void RenderGraphBuilder::swap(RenderGraphBuilder& other) noexcept
    {
        std::swap(scope_, other.scope_);
        error_.swap(other.error_);
        resources_.swap(other.resources_);
        passes_.swap(other.passes_);
        order_.swap(other.order_);
        outputs_.swap(other.outputs_);
        providers_.swap(other.providers_);
    }

    RenderResult<RenderGraphDefinition> RenderGraphBuilder::finish() && noexcept
    {
        RenderGraphBuilder consumed(std::move(*this));
        if (consumed.error_)
        {
            return cxx::unexpected(*consumed.error_);
        }
        std::vector<GraphDependency> dependencies;
        for (const auto& [before, after] : consumed.order_)
        {
            GraphPassId a{}, b{};
            for (std::size_t i = 0; i < consumed.passes_.size(); ++i)
            {
                if (consumed.passes_[i].key == before)
                {
                    a = GraphPassId{static_cast<std::uint32_t>(i + 1)};
                }
                if (consumed.passes_[i].key == after)
                {
                    b = GraphPassId{static_cast<std::uint32_t>(i + 1)};
                }
            }
            if (!a.isValid() || !b.isValid())
            {
                return cxx::unexpected(RenderError{kGraphInvalidDependency, {before.value(), after.value()}});
            }
            dependencies.push_back({a, b});
        }
        return RenderGraphDefinition::create(
            std::move(consumed.resources_),
            std::move(consumed.passes_),
            std::move(dependencies),
            std::move(consumed.outputs_),
            std::move(consumed.providers_)
        );
    }

    GraphTexture RenderGraphBuilder::texture(TextureDesc description, std::string_view name) noexcept
    {
        GraphResource resource;

        resource.description = description;
        resource.canonical_name = name;
        resource.semantic = graphResourceKey(name);
        resources_.push_back(std::move(resource));
        return GraphTexture{static_cast<std::uint32_t>(resources_.size()), scope_};
    }

    GraphBuffer RenderGraphBuilder::buffer(BufferDesc description, std::string_view name) noexcept
    {
        GraphResource resource;
        resource.description = description;
        resource.canonical_name = name;
        resource.semantic = graphResourceKey(name);
        resources_.push_back(std::move(resource));
        return GraphBuffer{static_cast<std::uint32_t>(resources_.size()), scope_};
    }

    GraphTexture RenderGraphBuilder::importTexture(
        std::string_view name,
        TextureDesc description,
        EPersistentScope scope,
        GraphImportContract contract
    ) noexcept
    {
        const auto id = texture(description, name);
        if (name.empty() || scope == EPersistentScope::NONE)
        {
            error_ = RenderError{kGraphInvalidResource, {id.value()}};
        }
        resources_.back().origin = EGraphResourceOrigin::IMPORTED;
        resources_.back().persistent_scope = scope;
        resources_.back().import_contract = std::move(contract);
        return id;
    }

    GraphBuffer RenderGraphBuilder::importBuffer(
        std::string_view name,
        BufferDesc description,
        EPersistentScope scope,
        GraphImportContract contract
    ) noexcept
    {
        const auto id = buffer(description, name);
        if (name.empty() || scope == EPersistentScope::NONE)
        {
            error_ = RenderError{kGraphInvalidResource, {id.value()}};
        }
        resources_.back().origin = EGraphResourceOrigin::IMPORTED;
        resources_.back().persistent_scope = scope;
        resources_.back().import_contract = std::move(contract);
        return id;
    }

    GraphTexture RenderGraphBuilder::importTarget(
        std::string_view name,
        TextureDesc description,
        RenderTargetSemanticId target,
        GraphImportContract contract
    ) noexcept
    {
        const auto id = importTexture(name, description, EPersistentScope::VIEW, std::move(contract));
        if (!target.isValid())
        {
            error_ = RenderError{kGraphInvalidResource, {id.value()}};
        }
        resources_.back().target_semantic = target;
        return id;
    }

    RenderResult<GraphPassId> RenderGraphBuilder::addCapturedPass(
        std::string_view name,
        ShaderReference shader_reference,
        EPassKind kind,
        EExecutionScope scope,
        rdesc::PassShaderContract contract,
        detail::CapturedParameters captured
    ) noexcept
    {
        for (const auto& field : contract.resources)
        {
            const auto role = field.role;
            const bool is_attachment =
                role == rdesc::EPassFieldRole::COLOR_ATTACHMENT || role == rdesc::EPassFieldRole::DEPTH_STENCIL ||
                role == rdesc::EPassFieldRole::RESOLVE || role == rdesc::EPassFieldRole::INPUT_ATTACHMENT;
            const bool is_transfer =
                role == rdesc::EPassFieldRole::TRANSFER_SOURCE || role == rdesc::EPassFieldRole::TRANSFER_DESTINATION;
            const bool is_invalid_attachment = is_attachment && kind != EPassKind::GRAPHICS;
            const auto stage_mask = kind == EPassKind::COMPUTE ? 4u : 3u;
            const bool is_shader_field = rdesc::isShaderDescriptorRole(role);
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
        if (captured.error)
        {
            return cxx::unexpected(*captured.error);
        }
        if (captured.authoring_scopes.size() != captured.uses.size())
        {
            return cxx::unexpected(RenderError{kGraphInvalidUse, {passes_.size() + 1, 0}});
        }
        for (std::size_t index = 0; index < captured.uses.size(); ++index)
        {
            const auto& use = captured.uses[index];
            const bool is_foreign_scope = captured.authoring_scopes[index] != scope_;
            const bool is_invalid_position = !use.resource.isValid() || use.resource.value() > resources_.size();
            const bool is_invalid_field = use.field_index >= captured.bindings.size();
            if (is_foreign_scope || is_invalid_position || is_invalid_field)
            {
                return cxx::unexpected(RenderError{kGraphInvalidUse, {passes_.size() + 1, use.resource.value()}});
            }
            const auto kind = captured.bindings[use.field_index].resource_kind;
            if (kind != resources_[use.resource.value() - 1].kind())
            {
                return cxx::unexpected(RenderError{kGraphInvalidUse, {passes_.size() + 1, use.resource.value()}});
            }
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

    RenderResult<void> RenderGraphBuilder::after(PassKey before, PassKey after) noexcept
    {
        if (!before.isValid() || !after.isValid() || before == after)
        {
            return cxx::unexpected(RenderError{kGraphInvalidDependency, {before.value(), after.value()}});
        }
        order_.emplace_back(before, after);
        return {};
    }

    RenderResult<void> RenderGraphBuilder::source(
        PassKey key,
        std::string_view field,
        VGraphProducer producer,
        std::optional<AuthoringFallback> fallback,
        std::uint32_t array_element
    ) noexcept
    {
        std::optional<GraphFallback> resolved;
        if (fallback)
        {
            bool valid = false;
            std::visit(
                [&](const auto& resource)
                {
                    constexpr bool image = std::is_same_v<std::decay_t<decltype(resource)>, GraphTexture>;
                    valid = resource.authoringScope() == scope_ && resource.isValid() &&
                            resource.value() <= resources_.size() &&
                            resources_[resource.value() - 1].kind() ==
                                (image ? EGraphResourceKind::IMAGE : EGraphResourceKind::BUFFER);
                    if (valid)
                    {
                        resolved = GraphFallback{GraphResourceId{resource.value()}, fallback->producer};
                    }
                },
                fallback->resource
            );
            if (!valid)
            {
                return cxx::unexpected(RenderError{kGraphInvalidResource, {}});
            }
        }
        for (auto& pass : passes_)
        {
            if (pass.key == key)
            {
                for (auto& use : pass.uses)
                {
                    if (pass.bindings[use.field_index].path == field &&
                        pass.bindings[use.field_index].array_element == array_element)
                    {
                        use.producer = producer;
                        use.fallback = resolved;
                        return {};
                    }
                }
            }
        }
        return cxx::unexpected(RenderError{kGraphInvalidUse, {key.value(), 0}});
    }

    RenderResult<void> RenderGraphBuilder::condition(PassKey key, GraphResourceKey group) noexcept
    {
        for (auto& pass : passes_)
        {
            if (pass.key == key)
            {
                pass.condition = group;
                return {};
            }
        }
        return cxx::unexpected(RenderError{kGraphInvalidUse, {key.value(), 0}});
    }

    RenderResult<void> RenderGraphBuilder::invocationInputs(PassKey key, std::uint32_t mask) noexcept
    {
        if ((mask & ~15u) != 0)
        {
            return cxx::unexpected(RenderError{kGraphInvalidUse, {key.value(), mask}});
        }
        for (auto& pass : passes_)
        {
            if (pass.key == key)
            {
                pass.invocation_inputs = mask;
                return {};
            }
        }
        return cxx::unexpected(RenderError{kGraphInvalidUse, {key.value(), 0}});
    }

    RenderResult<void> RenderGraphBuilder::provide(
        GraphResourceKey semantic,
        GraphTexture resource,
        PassKey pass
    ) noexcept
    {
        const bool invalid = !semantic.isValid() || !pass.isValid() || resource.authoringScope() != scope_ ||
                             !resource.isValid() || resource.value() > resources_.size() ||
                             resources_[resource.value() - 1].kind() != EGraphResourceKind::IMAGE;
        if (invalid)
        {
            return cxx::unexpected(RenderError{kGraphInvalidResource, {resource.value()}});
        }
        providers_.push_back({semantic, GraphResourceId{resource.value()}, pass});
        return {};
    }

    RenderResult<void> RenderGraphBuilder::provide(
        GraphResourceKey semantic,
        GraphBuffer resource,
        PassKey pass
    ) noexcept
    {
        const bool invalid = !semantic.isValid() || !pass.isValid() || resource.authoringScope() != scope_ ||
                             !resource.isValid() || resource.value() > resources_.size() ||
                             resources_[resource.value() - 1].kind() != EGraphResourceKind::BUFFER;
        if (invalid)
        {
            return cxx::unexpected(RenderError{kGraphInvalidResource, {resource.value()}});
        }
        providers_.push_back({semantic, GraphResourceId{resource.value()}, pass});
        return {};
    }

    RenderResult<void> RenderGraphBuilder::localRead(PassKey key, std::string_view field) noexcept
    {
        for (auto& pass : passes_)
        {
            if (pass.key == key)
            {
                for (auto& use : pass.uses)
                {
                    if (pass.bindings[use.field_index].path == field)
                    {
                        if (use.usage != EGraphUsage::INPUT_ATTACHMENT)
                        {
                            return cxx::unexpected(RenderError{kGraphInvalidUse, {key.value(), use.resource.value()}});
                        }
                        use.local_read = true;
                        return {};
                    }
                }
            }
        }
        return cxx::unexpected(RenderError{kGraphInvalidUse, {key.value()}});
    }

    RenderResult<void> RenderGraphBuilder::exportTexture(
        GraphTexture resource,
        VGraphProducer producer,
        EGraphOutput kind,
        ImageRange range,
        GraphResourceKey semantic
    ) noexcept
    {
        if (resource.authoringScope() != scope_ || !resource.isValid() || resource.value() > resources_.size() ||
            resources_[resource.value() - 1].kind() != EGraphResourceKind::IMAGE)
        {
            return cxx::unexpected(RenderError{kGraphInvalidResource, {resource.value()}});
        }
        outputs_.push_back({GraphResourceId{resource.value()}, producer, range, kind, semantic});
        return {};
    }

    RenderResult<void> RenderGraphBuilder::exportBuffer(
        GraphBuffer resource,
        VGraphProducer producer,
        EGraphOutput kind,
        BufferRange range,
        GraphResourceKey semantic
    ) noexcept
    {
        if (resource.authoringScope() != scope_ || !resource.isValid() || resource.value() > resources_.size() ||
            resources_[resource.value() - 1].kind() != EGraphResourceKind::BUFFER)
        {
            return cxx::unexpected(RenderError{kGraphInvalidResource, {resource.value()}});
        }
        outputs_.push_back({GraphResourceId{resource.value()}, producer, range, kind, semantic});
        return {};
    }

    EGraphAccess detail::resourceAccess(rdesc::EPassFieldRole role) noexcept
    {
        using ERole = rdesc::EPassFieldRole;
        switch (role)
        {
        case ERole::STORAGE_WRITE:
        case ERole::COLOR_ATTACHMENT:
        case ERole::DEPTH_STENCIL:
        case ERole::RESOLVE:
        case ERole::TRANSFER_DESTINATION:
            return EGraphAccess::WRITE;
        case ERole::STORAGE_READ_WRITE:
        case ERole::READ_WRITE_STORAGE:
            return EGraphAccess::READ_WRITE;
        default:
            return EGraphAccess::READ;
        }
    }

    EGraphUsage detail::resourceUsage(rdesc::EPassFieldRole role) noexcept
    {
        using ERole = rdesc::EPassFieldRole;
        switch (role)
        {
        case ERole::COLOR_ATTACHMENT:
            return EGraphUsage::COLOR_ATTACHMENT;
        case ERole::DEPTH_STENCIL:
            return EGraphUsage::DEPTH_ATTACHMENT;
        case ERole::RESOLVE:
            return EGraphUsage::RESOLVE;
        case ERole::TRANSFER_SOURCE:
        case ERole::TRANSFER_DESTINATION:
            return EGraphUsage::TRANSFER;
        case ERole::UNIFORM_READ:
            return EGraphUsage::UNIFORM;
        case ERole::VERTEX:
            return EGraphUsage::VERTEX;
        case ERole::INDEX:
            return EGraphUsage::INDEX;
        case ERole::INDIRECT:
            return EGraphUsage::INDIRECT;
        case ERole::INPUT_ATTACHMENT:
            return EGraphUsage::INPUT_ATTACHMENT;
        default:
            return EGraphUsage::SHADER;
        }
    }
} // namespace lux::render
