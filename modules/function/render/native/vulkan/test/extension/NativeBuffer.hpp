#pragma once

#include "Buffer.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>
#include <lux/engine/render/resource/Upload.hpp>

namespace extension_test
{
    namespace vk = lux::render::vulkan;

    // An external professional Storage: only public Render/Native extension contracts are used.
    class NativeBuffer final
    {
        struct Physical final
        {
            ~Physical() noexcept
            {
                if (resident) std::terminate();
            }
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
                --physical->resident;
            }
            std::shared_ptr<Physical> physical;
            std::shared_ptr<vk::Buffer> buffer;
            vk::SubmissionRequirements last_use;
        };
        struct Use;
        struct Entry final
        {
            std::shared_ptr<Backing> current;
            std::vector<Use*> uses;
            bool updating = false;
        };
        struct Use final
        {
            NativeBuffer* storage;
            BufferId id;
            std::size_t position;
            vk::PreparedStorageBinding& binding;
            std::array<float, 4> gain{};
            void update(const std::shared_ptr<Backing>& value) noexcept
            {
                binding.resources = value;
                binding.buffer = value->buffer.get();
                binding.last_use = &value->last_use;
                binding.publishUpdate();
            }
            ~Use() noexcept { if (storage) storage->unsubscribe(*this); }
        };
    public:
        NativeBuffer(vk::DeviceSession& session, StorageId id, TestBufferStorage::Config config)
            : session_(session), id_(id), config_(config), physical_(std::make_shared<Physical>())
        {
            entries_.reserve(config.capacity);
        }
        ~NativeBuffer() noexcept
        {
            for (auto& entry : entries_) disconnect(entry);
        }

        RenderResult<void> initialize() noexcept
        {
            if (!config_.capacity || config_.capacity > 64 || !config_.bytes) return lux::cxx::unexpected(RenderError{kStorageCapacity});
            return {};
        }

    private:
        friend struct lux::render::StorageTraits<NativeBuffer>;

        // Only the unpublished professional resource and its rollback belong here.
        struct Candidate final
        {
            NativeBuffer& storage;
            BufferId id;
            std::shared_ptr<Backing> backing;
            std::shared_ptr<void> storage_reference;

            ~Candidate() noexcept
            {
                if (backing)
                {
                    auto& entry = *storage.entries_.find(id.local);
                    entry.updating = false;
                    if (!entry.current) storage.entries_.erase(id.local);
                }
            }

            RenderResult<void> publish(const vk::SubmissionRequirements& submitted) noexcept
            {
                if (!storage.session_.resources().native(storage.id_))
                    return lux::cxx::unexpected(RenderError{kStorageIdentity});
                backing->last_use = submitted;
                auto& entry = *storage.entries_.find(id.local);
                entry.current = std::move(backing);
                entry.updating = false;
                for (auto* use : entry.uses) use->update(entry.current);
                return {};
            }
        };

        using UploadResult = exec::task<RenderResult<BufferBinding>>;
        UploadResult upload(SetBuffer::Request input) noexcept
        {
            const auto source = input.source.bytes().view();
            auto id = input.id;
            if (id.storage != id_) co_return lux::cxx::unexpected(RenderError{kStorageIdentity});
            if (source.size() != config_.bytes) co_return lux::cxx::unexpected(RenderError{kStorageContract});
            if (auto collected = session_.collect(); !collected) co_return lux::cxx::unexpected(collected.error());
            if (physical_->resident == config_.capacity * 2 || (id.isNull() && entries_.size() == config_.capacity))
                co_return lux::cxx::unexpected(RenderError{kStorageCapacity});
            if (!id.isNull() && !entries_.find(id.local)) co_return lux::cxx::unexpected(RenderError{kStorageIdentity});
            if (!id.isNull() && entries_.find(id.local)->updating) co_return lux::cxx::unexpected(RenderError{vk::kBusy});
            auto buffer = session_.allocator().createBuffer(source.size(),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                vk::EMemoryAccess::DEVICE);
            if (!buffer) co_return lux::cxx::unexpected(buffer.error());
            auto backing = std::make_shared<Backing>(physical_);
            backing->buffer = std::move(*buffer);
            if (id.isNull()) id = BufferId{id_, entries_.emplace()};
            entries_.find(id.local)->updating = true;
            auto storage_reference = session_.resources().share(id_);
            auto candidate = std::make_unique<Candidate>(*this, id, std::move(backing), std::move(*storage_reference));
            auto uploaded = co_await session_.uploads().writeBuffer(candidate->backing->buffer, std::move(input.source));
            if (!uploaded) co_return lux::cxx::unexpected(uploaded.error());
            const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
            if (stop.stop_requested()) co_return lux::cxx::unexpected(RenderError{vk::kWorkStopped});
            if (auto published = candidate->publish(*uploaded); !published)
                co_return lux::cxx::unexpected(published.error());
            co_return BufferBinding{id};
        }

    public:
        RenderResult<void> remove(BufferId id) noexcept
        {
            if (id.storage != id_) return lux::cxx::unexpected(RenderError{kStorageIdentity});
            auto* entry = entries_.find(id.local);
            if (!entry) return lux::cxx::unexpected(RenderError{kStorageIdentity});
            if (entry->updating) return lux::cxx::unexpected(RenderError{vk::kBusy});
            disconnect(*entry);
            entries_.erase(id.local);
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
            if (!description || description->size != config_.bytes) return lux::cxx::unexpected(RenderError{kStorageMismatch});
            if (argument.id.storage != id_) return lux::cxx::unexpected(RenderError{kStorageIdentity});
            auto* entry = entries_.find(argument.id.local);
            if (!entry || !entry->current) return lux::cxx::unexpected(RenderError{kStorageIdentity});
            auto use = std::make_unique<Use>(this, argument.id, entry->uses.size(), binding, argument.gain);
            static const ShaderBlock layout = [] {
                static constexpr lux::rdesc::DataGpuField fields[]{
                    {"gain", lux::rdesc::EDataScalar::FLOAT, 0, 4, 1, 0, 0}
                };
                return ShaderBlock::from({lux::rdesc::EDataLayout::STD430, 16, 16, fields, {}, {}, 0});
            }();
            binding.parameters = std::as_bytes(std::span(use->gain));
            binding.parameter_layout = &layout;
            entry->uses.push_back(use.get());
            use->update(entry->current);
            binding.reference = {use.release(), [](void* value) noexcept { delete static_cast<Use*>(value); }};
            return {};
        }
    private:
        void unsubscribe(Use& use) noexcept
        {
            auto& subscriptions = entries_.find(use.id.local)->uses;
            subscriptions[use.position] = subscriptions.back();
            subscriptions[use.position]->position = use.position;
            subscriptions.pop_back();
        }

        static void disconnect(Entry& entry) noexcept
        {
            for (auto* use : entry.uses) use->storage = nullptr;
            entry.uses.clear();
        }

        vk::DeviceSession& session_;
        StorageId id_;
        TestBufferStorage::Config config_;
        lux::cxx::SlotMap<Entry, BufferTag> entries_;
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
            extension_test::NativeBuffer& storage, extension_test::SetBuffer::Request input
        ) noexcept
        {
            return storage.upload(std::move(input));
        }
    };
}
