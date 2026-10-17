#pragma once

#include "Buffer.hpp"
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/transfer/Transfer.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>
#include <lux/engine/render/resource/Upload.hpp>

namespace extension_test
{
    namespace vk = lux::render::vulkan;

    // An external professional Storage: only public Render/Native extension contracts are used.
    class NativeBuffer final
    {
        struct Physical final
        {
            explicit Physical(vk::DeviceSession& value) noexcept : session(value) {}
            ~Physical() noexcept
            {
                if (resident) std::terminate();
            }
            vk::DeviceSession& session;
            std::size_t resident = 0;
        };
        struct Backing final
        {
            explicit Backing(std::shared_ptr<Physical> value) noexcept : physical(std::move(value))
            {
                ++physical->resident;
            }
            ~Backing() noexcept
            {
                if (buffer && !physical->session.releaseCompleted(buffer, last_use)) std::terminate();
                --physical->resident;
            }
            [[nodiscard]] VkDevice device() const noexcept { return physical->session.device().native(); }
            std::shared_ptr<Physical> physical;
            vk::RetirementReservation retirement;
            std::size_t position = 0;
            vk::BufferPermit buffer;
            vk::SubmissionRequirements last_use;
            unsigned references = 0;
            bool live = false;
        };
        struct Entry final { Backing* current = nullptr; bool updating = false; };
        struct Use final
        {
            NativeBuffer& storage;
            BufferId id;
            Backing* backing = nullptr;
            vk::PreparedStorageBinding& binding;
            void update(Backing& value) noexcept
            {
                if (backing) storage.releaseReference(*backing);
                backing = &value;
                ++backing->references;
                binding.buffer = storage.session_.borrow(value.buffer);
                binding.last_use = &value.last_use;
                binding.publishUpdate();
            }
            ~Use() noexcept { storage.releaseReference(*backing); std::erase(storage.uses_, this); }
        };
    public:
        NativeBuffer(vk::DeviceSession& session, StorageId id, TestBufferStorage::Config config)
            : session_(session), id_(id), config_(config), physical_(std::make_shared<Physical>(session))
        {
            entries_.reserve(config.capacity);
            backings_.reserve(config.capacity * 2);
        }
        ~NativeBuffer() noexcept
        {
            if (!uses_.empty()) std::terminate();
            while (!backings_.empty()) erase(backings_.back().get());
        }

        RenderResult<void> initialize() noexcept
        {
            if (!config_.capacity || config_.capacity > 64) return lux::cxx::unexpected(RenderError{kStorageCapacity});
            return {};
        }

    private:
        friend struct lux::render::StorageTraits<NativeBuffer>;

        // Only the professional transaction belongs here; generic Transfer owns asynchronous progress.
        struct Candidate final
        {
            NativeBuffer& storage;
            BufferId id;
            Backing* backing;
            StorageLease lease;
            bool published = false;

            ~Candidate() noexcept
            {
                if (!published)
                {
                    auto& entry = *storage.entries_.find(id.local);
                    entry.updating = false;
                    if (!entry.current) storage.entries_.erase(id.local);
                    storage.erase(backing);
                }
            }

            RenderResult<void> publish(vk::Transfer& transfer) noexcept
            {
                auto buffer = transfer.takeBuffer();
                if (!buffer) return lux::cxx::unexpected(buffer.error());
                backing->buffer = std::move(*buffer);
                backing->last_use = transfer.requirements();
                auto& entry = *storage.entries_.find(id.local);
                auto* previous = entry.current;
                entry.current = backing;
                entry.updating = false;
                backing->live = true;
                if (previous)
                {
                    previous->live = false;
                    storage.retireUnused(*previous);
                }
                for (auto* use : storage.uses_) if (use->id == id) use->update(*backing);
                published = true;
                return {};
            }
        };

        using UploadResult = RenderResult<std::pair<BufferBinding, vk::GpuWork>>;
        UploadResult upload(const SetBuffer::Request& input) noexcept
        {
            const auto source = input.source.bytes().view();
            auto id = input.id;
            if (id.storage != id_) return lux::cxx::unexpected(RenderError{kStorageIdentity});
            if (source.size() != 16) return lux::cxx::unexpected(RenderError{kStorageContract});
            if (auto collected = session_.collect(); !collected) return lux::cxx::unexpected(collected.error());
            if (physical_->resident == config_.capacity * 2 || (id.isNull() && entries_.size() == config_.capacity))
                return lux::cxx::unexpected(RenderError{kStorageCapacity});
            if (!id.isNull() && !entries_.find(id.local)) return lux::cxx::unexpected(RenderError{kStorageIdentity});
            if (!id.isNull() && entries_.find(id.local)->updating) return lux::cxx::unexpected(RenderError{vk::kBusy});
            auto transfer = session_.uploadBuffer(source, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            if (!transfer) return lux::cxx::unexpected(transfer.error());
            auto retirement = session_.retirement().reserve();
            if (!retirement) return lux::cxx::unexpected(retirement.error());
            auto backing = std::make_unique<Backing>(physical_);
            backing->retirement = std::move(*retirement);
            backing->position = backings_.size();
            if (id.isNull()) id = BufferId{id_, entries_.emplace()};
            entries_.find(id.local)->updating = true;
            auto* value = backing.get();
            backings_.push_back(std::move(backing));
            auto lease = session_.resources().lease(id_);
            auto candidate = std::make_unique<Candidate>(*this, id, value, std::move(*lease));
            auto work = vk::GpuWork::pending<[](Candidate& value, vk::Transfer& upload, vk::DeviceSession&) noexcept {
                return value.publish(upload);
            }>(session_, *transfer, candidate);
            if (!work) return lux::cxx::unexpected(work.error());
            return std::pair{BufferBinding{id}, std::move(*work)};
        }

    public:
        RenderResult<void> remove(BufferId id) noexcept
        {
            if (id.storage != id_) return lux::cxx::unexpected(RenderError{kStorageIdentity});
            auto* entry = entries_.find(id.local);
            if (!entry) return lux::cxx::unexpected(RenderError{kStorageIdentity});
            if (entry->updating) return lux::cxx::unexpected(RenderError{vk::kBusy});
            auto* value = entry->current;
            value->live = false;
            entries_.erase(id.local);
            retireUnused(*value);
            return {};
        }
        bool exists(BufferId id) const noexcept
        {
            const auto* value = id.storage == id_ ? entries_.find(id.local) : nullptr;
            return value && value->current;
        }
        std::size_t backingCount() const noexcept { return physical_->resident; }
        bool compatible(const TestBufferStorage::Config& config) const noexcept { return config == config_; }
        RenderResult<void> prepare(const vk::NativeResourcePlan& plan, const BufferBinding& argument,
            vk::PreparedStorageBinding& binding) noexcept
        {
            const auto* description = std::get_if<GraphBufferDesc>(&plan.description);
            if (!description || description->size != 16) return lux::cxx::unexpected(RenderError{kStorageMismatch});
            if (argument.id.storage != id_) return lux::cxx::unexpected(RenderError{kStorageIdentity});
            auto* entry = entries_.find(argument.id.local);
            if (!entry || !entry->current) return lux::cxx::unexpected(RenderError{kStorageIdentity});
            auto use = std::make_unique<Use>(*this, argument.id, nullptr, binding);
            use->update(*entry->current);
            uses_.push_back(use.get());
            binding.reference = {use.release(), [](void* value) noexcept { delete static_cast<Use*>(value); }};
            return {};
        }
    private:
        void releaseReference(Backing& value) noexcept
        {
            --value.references;
            retireUnused(value);
        }
        void retireUnused(Backing& value) noexcept
        {
            if (!value.live && !value.references) erase(&value);
        }
        void erase(Backing* value) noexcept
        {
            if (value->references) std::terminate();
            const auto position = value->position;
            auto owned = std::move(backings_[position]);
            if (position + 1 != backings_.size())
            {
                backings_[position] = std::move(backings_.back());
                backings_[position]->position = position;
            }
            backings_.pop_back();
            if (owned->last_use.completed()) return;
            auto result = session_.retirement().retire(owned->retirement, owned->last_use, owned);
            if (!result) std::terminate();
        }
        vk::DeviceSession& session_;
        StorageId id_;
        TestBufferStorage::Config config_;
        lux::cxx::SlotMap<Entry, BufferTag> entries_;
        std::vector<std::unique_ptr<Backing>> backings_;
        std::vector<Use*> uses_;
        std::shared_ptr<Physical> physical_;
    };

    inline StorageFactory bufferFactory() noexcept
    {
        static const TestBufferStorage::Ops ops{[](const void* object, BufferId id) noexcept {
            return static_cast<const NativeBuffer*>(object)->exists(id);
        }};
        static const vk::StorageBindingOps binding{
            [](void* object, const vk::NativeResourcePlan& plan, const void* argument,
                vk::PreparedStorageBinding& binding) noexcept {
                return static_cast<NativeBuffer*>(object)->prepare(plan, *static_cast<const BufferBinding*>(argument), binding);
            }
        };
        static const auto create = +[](vk::DeviceSession& session, StorageId id, const TestBufferStorage::Config& config) noexcept
            -> RenderResult<std::unique_ptr<NativeBuffer>> {
            auto result = std::make_unique<NativeBuffer>(session, id, config);
            if (auto ready = result->initialize(); !ready) return lux::cxx::unexpected(ready.error());
            return result;
        };
        static const auto compatible = +[](const NativeBuffer& value, const TestBufferStorage::Config& config) noexcept {
            return value.compatible(config);
        };
        return makeStorageFactory<TestBufferStorage, NativeBuffer, vk::DeviceSession, create, compatible>(
            ops, &binding);
    }
}

namespace lux::render
{
    template <> struct StorageTraits<extension_test::NativeBuffer> final
    {
        static extension_test::NativeBuffer::UploadResult upload(
            extension_test::NativeBuffer& storage, const extension_test::SetBuffer::Request& input
        ) noexcept
        {
            return storage.upload(input);
        }
    };
}
