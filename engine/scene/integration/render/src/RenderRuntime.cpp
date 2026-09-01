#include <lux/engine/scene/RenderRuntime.hpp>

#include <utility>

namespace lux::scene
{
    RenderRuntimeLease::RenderRuntimeLease(RenderRuntime& owner) noexcept : owner_(&owner)
    {
    }

    RenderRuntimeLease::~RenderRuntimeLease() noexcept
    {
        reset();
    }

    RenderRuntimeLease::RenderRuntimeLease(RenderRuntimeLease&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr))
    {
    }

    RenderRuntimeLease& RenderRuntimeLease::operator=(RenderRuntimeLease&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            owner_ = std::exchange(other.owner_, nullptr);
        }
        return *this;
    }

    RenderRuntimeLease::operator bool() const noexcept
    {
        return owner_ != nullptr;
    }

    render::RenderControlSession& RenderRuntimeLease::control() noexcept
    {
        return owner_->control();
    }

    render::RenderProgramSession& RenderRuntimeLease::programs() noexcept
    {
        return owner_->programs();
    }

    render::RenderUploadClient RenderRuntimeLease::upload() noexcept
    {
        return owner_->upload();
    }

    const render::FeatureCatalog& RenderRuntimeLease::features() const noexcept
    {
        return owner_->features();
    }

    void RenderRuntimeLease::reset() noexcept
    {
        if (owner_ != nullptr)
        {
            auto* owner = std::exchange(owner_, nullptr);
            owner->release();
        }
    }

    RenderRuntimeLease RenderRuntime::makeLease() noexcept
    {
        return RenderRuntimeLease(*this);
    }
} // namespace lux::scene
