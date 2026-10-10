/**
 * @file VertexPoolRegistry.cpp
 */

#include <lux/engine/render/core/RenderErrorSink.hpp>
#include <lux/engine/render/resources/vertex/IVertexSource.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>
#include <lux/engine/render/resources/vertex/VertexRegistration.hpp>

#include <utility>

#include <lux/engine/render/gpu/VulkanContext.hpp> // DeviceContext

namespace lux::render
{
    VertexPoolRegistry::CreateResult VertexPoolRegistry::create(
        DeviceContext& device,
        std::span<const VkDescriptorSet> sets,
        std::uint32_t binding_offset,
        RenderErrorSink* error_sink
    ) noexcept
    {
        const bool is_invalid_count = sets.empty() || sets.size() > UINT32_MAX;
        const bool has_missing_set = std::ranges::any_of(sets, [](VkDescriptorSet set) { return !set; });
        const bool is_invalid_target = is_invalid_count || has_missing_set;
        if (is_invalid_target)
        {
            return renderFailure<err::descriptor::InvalidVertexPoolTarget>();
        }
        DomainWriteTarget domain;
        if (auto accepted = domain.set(sets, binding_offset); !accepted)
        {
            return lux::cxx::unexpected(accepted.error());
        }
        return std::unique_ptr<VertexPoolRegistry>(
            new VertexPoolRegistry(device.logicalDevice(), std::move(domain), error_sink)
        );
    }

    VertexPoolRegistry::VertexPoolRegistry(
        VkDevice device,
        DomainWriteTarget domain,
        RenderErrorSink* error_sink
    ) noexcept
        : device_(device), domain_(std::move(domain)), error_sink_(error_sink)
    {
    }

    detail::VertexRegistration::VertexRegistration(
        VertexPoolRegistry& owner,
        IVertexSource& target,
        std::uint32_t id
    ) noexcept
        : registry(&owner), source(&target), slot(id)
    {
    }

    detail::VertexRegistration::~VertexRegistration() noexcept
    {
        revoke();
    }

    void detail::VertexRegistration::revoke(bool source_destroying) noexcept
    {
        if (!registry)
        {
            return;
        }
        registry->slots_[slot] = nullptr;
        auto* target = std::exchange(source, nullptr);
        target->registration_ = nullptr;
        registry = nullptr;
        if (!source_destroying)
        {
            target->setBindlessPoolId(~0u);
        }
    }

    VertexSourceRegistration::VertexSourceRegistration() noexcept = default;

    VertexSourceRegistration::VertexSourceRegistration(
        std::unique_ptr<detail::VertexRegistration> registration
    ) noexcept
        : registration_(std::move(registration))
    {
    }

    VertexSourceRegistration::~VertexSourceRegistration() noexcept = default;

    VertexSourceRegistration::VertexSourceRegistration(VertexSourceRegistration&&) noexcept = default;

    VertexSourceRegistration& VertexSourceRegistration::operator=(VertexSourceRegistration&& other) noexcept
    {
        if (this != &other)
        {
            registration_ = std::move(other.registration_);
        }
        return *this;
    }

    VertexSourceRegistration::operator bool() const noexcept
    {
        return registration_ && registration_->registry;
    }

    std::uint32_t VertexSourceRegistration::poolId() const noexcept
    {
        return *this ? registration_->slot : ~0u;
    }

    void VertexSourceRegistration::refresh() noexcept
    {
        if (*this)
        {
            registration_->registry->writeDescriptor(registration_->slot, *registration_->source);
        }
    }

    VertexPoolRegistry::~VertexPoolRegistry() noexcept
    {
        for (auto* registration : slots_)
        {
            if (registration)
            {
                registration->revoke();
            }
        }
    }

    Expected<VertexSourceRegistration> VertexPoolRegistry::registerSource(IVertexSource& source) noexcept
    {
        if (source.registration_)
        {
            return renderFailure<err::descriptor::VertexSourceAlreadyRegistered>();
        }
        for (std::uint32_t slot = 0; slot < kVertexPoolMaxCount; ++slot)
        {
            if (!slots_[slot])
            {
                auto registration = std::make_unique<detail::VertexRegistration>(*this, source, slot);
                slots_[slot] = registration.get();
                source.registration_ = registration.get();
                source.setBindlessPoolId(slot);
                writeDescriptor(slot, source);
                return VertexSourceRegistration(std::move(registration));
            }
        }
        if (error_sink_)
        {
            error_sink_->emit(
                renderError<err::frame::VertexPoolRegistryFull>(kVertexPoolMaxCount),
                RenderErrorEvent::kNoScene,
                0
            );
        }
        return renderFailure<err::frame::VertexPoolRegistryFull>(kVertexPoolMaxCount);
    }

    bool VertexPoolRegistry::isRegistered(std::uint32_t pool_id) const noexcept
    {
        return pool_id < kVertexPoolMaxCount && slots_[pool_id];
    }

    void VertexPoolRegistry::writeDescriptor(std::uint32_t pool_id, IVertexSource& source) noexcept
    {
        VkBuffer buf = source.buffer();
        if (buf == VK_NULL_HANDLE)
        {
            // The slot remains reserved. Its owner refreshes after backing becomes ready;
            // no shader may address it until a valid descriptor has been published.
            return;
        }

        VkDescriptorBufferInfo info{};
        info.buffer = buf;
        info.offset = 0;
        info.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstArrayElement = pool_id;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.descriptorCount = 1;
        w.pBufferInfo = &info;

        // 阶段 C:域集是唯一写目标(legacy per-set 半边已删)。本集只有
        // 一个 binding,但它是数组(容量 kVertexPoolMaxCount),所以位移的是
        // dstBinding,dstArrayElement 保持 pool_id 不变。本资源只有一个集,
        // 而域集是逐 slice 的 —— 每个 slice 都要写。
        for (uint32_t s = 0; s < domain_.sliceCount(); ++s)
        {
            VkDescriptorSet domain_ds = domain_.setFor(s);
            w.dstSet = domain_ds;
            w.dstBinding = domain_.binding(static_cast<uint32_t>(EVertexPoolSetBindings::VERTEX_POOLS));
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        }
    }

} // namespace lux::render
