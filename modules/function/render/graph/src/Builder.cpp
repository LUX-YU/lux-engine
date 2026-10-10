#include <lux/engine/render/graph/Builder.hpp>

namespace lux::render
{
    GraphTexture RenderGraphBuilder::texture(TextureDesc description) noexcept
    {
        GraphResource resource;
        resource.kind = EGraphResourceKind::IMAGE;
        resource.texture = description;
        resources_.push_back(resource);
        return GraphTexture{static_cast<std::uint32_t>(resources_.size())};
    }

    GraphBuffer RenderGraphBuilder::buffer(BufferDesc description) noexcept
    {
        GraphResource resource;
        resource.buffer = description;
        resources_.push_back(resource);
        return GraphBuffer{static_cast<std::uint32_t>(resources_.size())};
    }

    GraphTexture RenderGraphBuilder::importTexture(TextureDesc description, EPersistentScope scope) noexcept
    {
        const auto id = texture(description);
        resources_.back().origin = EGraphResourceOrigin::IMPORTED;
        resources_.back().persistent_scope = scope;
        return id;
    }

    GraphBuffer RenderGraphBuilder::importBuffer(BufferDesc description, EPersistentScope scope) noexcept
    {
        const auto id = buffer(description);
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
