#pragma once

#include "Buffer.hpp"
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/transfer/Transfer.hpp>

namespace extension_test
{
    namespace vk = lux::render::vulkan;

    // An external professional Storage: only public Render/Native extension contracts are used.
    class NativeBuffer final
    {
        struct Backing final
        {
            vk::BufferPermit buffer;
            VkDescriptorSet descriptors = VK_NULL_HANDLE;
            std::optional<vk::SubmissionTicket> last_use;
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
                if (backing) --backing->references;
                backing = &value;
                ++backing->references;
                binding.buffer = storage.session_.borrow(value.buffer);
                binding.descriptors = value.descriptors;
                binding.last_use = &value.last_use;
            }
            ~Use() noexcept { --backing->references; std::erase(storage.uses_, this); }
        };
    public:
        NativeBuffer(vk::DeviceSession& session, TestBufferStorage::Config config) : session_(session), config_(config)
        {
            entries_.reserve(config.capacity);
            backings_.reserve(config.capacity * 2);
        }
        ~NativeBuffer() noexcept
        {
            if (!uses_.empty()) std::terminate();
            for (auto& value : backings_) destroy(*value);
            if (pool_) vkDestroyDescriptorPool(session_.device().native(), pool_, nullptr);
            if (layout_) vkDestroyDescriptorSetLayout(session_.device().native(), layout_, nullptr);
        }

        RenderResult<void> initialize() noexcept
        {
            if (!config_.capacity || config_.capacity > 64) return lux::cxx::unexpected(RenderError{kStorageCapacity});
            const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layout.bindingCount = 1;
            layout.pBindings = &binding;
            auto result = vkCreateDescriptorSetLayout(session_.device().native(), &layout, nullptr, &layout_);
            if (result != VK_SUCCESS) return lux::cxx::unexpected(vk::nativeError(result));
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, config_.capacity * 2};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            pool.maxSets = size.descriptorCount;
            pool.poolSizeCount = 1;
            pool.pPoolSizes = &size;
            result = vkCreateDescriptorPool(session_.device().native(), &pool, nullptr, &pool_);
            return result == VK_SUCCESS ? RenderResult<void>{} : lux::cxx::unexpected(vk::nativeError(result));
        }

        class Upload final
        {
        public:
            NativeBuffer& storage;
            BufferId id;
            std::unique_ptr<vk::Transfer> transfer;
            Backing* candidate;
            StorageInstanceId storage_id;
            bool published = false;
            ~Upload() noexcept
            {
                if (!published)
                {
                    auto& entry = *storage.entries_.find(id);
                    entry.updating = false;
                    if (!entry.current) storage.entries_.erase(id);
                    storage.erase(candidate);
                }
                transfer.reset();
                storage.session_.resources().release(storage_id);
            }
            RenderResult<bool> advance() noexcept { return transfer->advance(); }
            void cancel() noexcept { transfer->cancel(); }
            bool unstartedIn(const vk::DeviceSession& session) const noexcept { return transfer->unstartedIn(session); }
            std::optional<vk::SubmissionTicket> lastUse() const noexcept { return transfer->lastUse(); }
            RenderResult<void> publish() noexcept
            {
                auto buffer = transfer->takeBuffer();
                if (!buffer) return lux::cxx::unexpected(buffer.error());
                candidate->buffer = std::move(*buffer);
                candidate->last_use = transfer->lastUse();
                VkDescriptorBufferInfo info{storage.session_.borrow(candidate->buffer)->native(), 0, 16};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = candidate->descriptors;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &info;
                vkUpdateDescriptorSets(storage.session_.device().native(), 1, &write, 0, nullptr);
                auto& entry = *storage.entries_.find(id);
                if (entry.current) entry.current->live = false;
                entry.current = candidate;
                entry.updating = false;
                candidate->live = true;
                // Existing prepared users retain their old backing until detach; new Attach sees this publication.
                published = true;
                return {};
            }
        };

        RenderResult<std::unique_ptr<Upload>> upload(std::span<const std::byte> source, BufferId id = {}) noexcept
        {
            if (source.size() != 16) return lux::cxx::unexpected(RenderError{kStorageContract});
            collect();
            if (backings_.size() == config_.capacity * 2 || (id.isNull() && entries_.size() == config_.capacity))
                return lux::cxx::unexpected(RenderError{kStorageCapacity});
            if (!id.isNull() && !entries_.find(id)) return lux::cxx::unexpected(RenderError{kStorageDomain});
            if (!id.isNull() && entries_.find(id)->updating) return lux::cxx::unexpected(RenderError{vk::kBusy});
            auto transfer = session_.uploadBuffer(source, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            if (!transfer) return lux::cxx::unexpected(transfer.error());
            auto backing = std::make_unique<Backing>();
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocate.descriptorPool = pool_;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &layout_;
            const auto result = vkAllocateDescriptorSets(session_.device().native(), &allocate, &backing->descriptors);
            if (result != VK_SUCCESS) return lux::cxx::unexpected(vk::nativeError(result));
            if (id.isNull()) id = entries_.emplace();
            entries_.find(id)->updating = true;
            auto* candidate = backing.get();
            backings_.push_back(std::move(backing));
            const auto storage_id = session_.resources().identityOf(this);
            session_.resources().retain(storage_id);
            return std::make_unique<Upload>(*this, id, std::move(*transfer), candidate, storage_id);
        }

        RenderResult<void> remove(BufferId id) noexcept
        {
            auto* entry = entries_.find(id);
            if (!entry) return lux::cxx::unexpected(RenderError{kStorageDomain});
            if (entry->updating) return lux::cxx::unexpected(RenderError{vk::kBusy});
            entry->current->live = false;
            entries_.erase(id);
            collect();
            return {};
        }
        bool exists(BufferId id) const noexcept { const auto* value = entries_.find(id); return value && value->current; }
        std::size_t backingCount() const noexcept { return backings_.size(); }
        bool compatible(const TestBufferStorage::Config& config) const noexcept { return config == config_; }
        void collect() noexcept
        {
            for (std::size_t i = backings_.size(); i-- > 0;)
            {
                auto* value = backings_[i].get();
                if (!value->live && !value->references && value->buffer && vk::RetirementQueue::completed(*value->last_use))
                    erase(value);
            }
        }
        RenderResult<void> prepare(const vk::ResourcePlan& plan, std::span<const std::byte> bytes,
            vk::PreparedStorageBinding& binding) noexcept
        {
            const auto* description = std::get_if<GraphBufferDesc>(&plan.description);
            if (!description || description->size != 16) return lux::cxx::unexpected(RenderError{kStorageMismatch});
            lux::cxx::BinaryReader reader(bytes, bytes.size());
            auto argument = DataSchema<BufferBinding>::decode(reader, BorrowedWireView{DataSchema<BufferBinding>::schema, bytes, {}});
            auto* entry = entries_.find(argument->id);
            if (!entry || !entry->current) return lux::cxx::unexpected(RenderError{kStorageDomain});
            auto use = std::make_unique<Use>(*this, argument->id, nullptr, binding);
            use->update(*entry->current);
            uses_.push_back(use.get());
            binding.reference = {use.release(), [](void* value) noexcept { delete static_cast<Use*>(value); }};
            return {};
        }
    private:
        void destroy(Backing& value) noexcept
        {
            if (value.references) std::terminate();
            if (value.descriptors) vkFreeDescriptorSets(session_.device().native(), pool_, 1, &value.descriptors);
            if (value.buffer)
                if (!session_.releaseCompleted(value.buffer, *value.last_use)) std::terminate();
        }
        void erase(Backing* value) noexcept
        {
            destroy(*value);
            std::erase_if(backings_, [&](const auto& item) { return item.get() == value; });
        }
        vk::DeviceSession& session_;
        TestBufferStorage::Config config_;
        lux::cxx::SlotMap<Entry, BufferTag> entries_;
        std::vector<std::unique_ptr<Backing>> backings_;
        std::vector<Use*> uses_;
        VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
        VkDescriptorPool pool_ = VK_NULL_HANDLE;
    };

    inline StorageFactory bufferFactory() noexcept
    {
        static const TestBufferStorage::Ops ops{[](const void* object, BufferId id) noexcept {
            return static_cast<const NativeBuffer*>(object)->exists(id);
        }};
        static const vk::StorageBindingOps binding{
            [](void* object, const vk::ResourcePlan& plan, std::span<const std::byte> argument,
                vk::PreparedStorageBinding& binding) noexcept {
                return static_cast<NativeBuffer*>(object)->prepare(plan, argument, binding);
            }
        };
        return {&TestBufferStorage::description, EGraphicsApi::VULKAN, &ops,
            [](void* session, const void* config) noexcept -> RenderResult<NativeStorage> {
                auto result = std::make_unique<NativeBuffer>(*static_cast<vk::DeviceSession*>(session),
                    *static_cast<const TestBufferStorage::Config*>(config));
                if (auto ready = result->initialize(); !ready) return lux::cxx::unexpected(ready.error());
                return NativeStorage{result.release(), [](void* value) noexcept { delete static_cast<NativeBuffer*>(value); }};
            },
            [](const void* object, const void* config) noexcept {
                return static_cast<const NativeBuffer*>(object)->compatible(*static_cast<const TestBufferStorage::Config*>(config));
            }, {}, &binding, [](void* object) noexcept { static_cast<NativeBuffer*>(object)->collect(); }
        };
    }
}
