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
    }

    RenderResult<RenderGraphDefinition> RenderGraphBuilder::finish() && noexcept
    {
        RenderGraphBuilder consumed(std::move(*this));
        if (consumed.error_)
        {
            return cxx::unexpected(*consumed.error_);
        }
        return RenderGraphDefinition::create(std::move(consumed.resources_), std::move(consumed.passes_));
    }

    GraphTexture RenderGraphBuilder::texture(TextureDesc description, std::string_view name) noexcept
    {
        GraphResource resource;
        resource.kind = EGraphResourceKind::IMAGE;
        resource.texture = description;
        resource.canonical_name = name;
        resource.semantic = graphResourceKey(name);
        resources_.push_back(std::move(resource));
        return GraphTexture{static_cast<std::uint32_t>(resources_.size()), scope_};
    }

    GraphBuffer RenderGraphBuilder::buffer(BufferDesc description, std::string_view name) noexcept
    {
        GraphResource resource;
        resource.buffer = description;
        resource.canonical_name = name;
        resource.semantic = graphResourceKey(name);
        resources_.push_back(std::move(resource));
        return GraphBuffer{static_cast<std::uint32_t>(resources_.size()), scope_};
    }

    GraphTexture RenderGraphBuilder::importTexture(
        std::string_view name,
        TextureDesc description,
        EPersistentScope scope
    ) noexcept
    {
        const auto id = texture(description, name);
        if (name.empty() || scope == EPersistentScope::NONE)
        {
            error_ = RenderError{kGraphInvalidResource, {id.value()}};
        }
        resources_.back().origin = EGraphResourceOrigin::IMPORTED;
        resources_.back().persistent_scope = scope;
        return id;
    }

    GraphBuffer RenderGraphBuilder::importBuffer(
        std::string_view name,
        BufferDesc description,
        EPersistentScope scope
    ) noexcept
    {
        const auto id = buffer(description, name);
        if (name.empty() || scope == EPersistentScope::NONE)
        {
            error_ = RenderError{kGraphInvalidResource, {id.value()}};
        }
        resources_.back().origin = EGraphResourceOrigin::IMPORTED;
        resources_.back().persistent_scope = scope;
        return id;
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
