#include "MeshDraws.hpp"
#include <bit>

namespace lux::render::vulkan
{
    MeshDraws::MeshDraws(
        DeviceSession& session,
        MeshInstanceStorage& storage,
        MeshInstanceDrawGroup selection,
        PreparedStorageBinding& binding
    ) noexcept
        : session_(session), storage_(&storage), selection_(selection), binding_(binding),
          retirement_(session.retirement().reserve())
    {
    }

    MeshDraws::~MeshDraws() noexcept
    {
        if (storage_)
        {
            auto found = storage_->draw_subscribers_.find(selection_);
            auto& uses = found->second;
            uses[position_] = uses.back();
            uses[position_]->position_ = position_;
            uses.pop_back();
            if (uses.empty())
            {
                storage_->draw_subscribers_.erase(found);
            }
        }
        if (buffer_ && !session_.retirement().retire(retirement_, buffer_))
        {
            std::terminate();
        }
    }

    void MeshDraws::changed() noexcept
    {
        binding_.pending_bytes = std::as_bytes(std::span(words_));
        binding_.publishUpdate();
    }

    void MeshDraws::added(std::uint32_t index) noexcept
    {
        positions_[index] = words_[1];
        words_[5 + words_[1]++] = index;
        changed();
    }

    void MeshDraws::removed(std::uint32_t index) noexcept
    {
        const auto position = positions_[index];
        const auto last = words_[5 + --words_[1]];
        words_[5 + position] = last;
        positions_[last] = position;
        positions_[index] = UINT32_MAX;
        changed();
    }

    RenderResult<void> MeshDraws::prepare(
        MeshInstanceStorage& storage,
        const MeshInstanceBinding& input,
        PreparedStorageBinding& binding
    ) noexcept
    {
        auto& registry = storage.session_.resources();
        auto mesh = registry.tryGet<render::MeshStorage>(input.group->mesh.storage);
        if (!mesh)
        {
            return cxx::unexpected(mesh.error());
        }
        auto info = mesh->query(input.group->mesh);
        if (!info)
        {
            return cxx::unexpected(info.error());
        }
        if (input.group->submesh >= info->description.submeshes.size())
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        auto material = registry.tryGet<render::MaterialStorage>(input.group->material.storage);
        if (!material)
        {
            return cxx::unexpected(material.error());
        }
        if (auto valid = material->query(input.group->material); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        auto value = std::unique_ptr<MeshDraws>(new MeshDraws(storage.session_, storage, *input.group, binding));
        // Registration follows successful preparation; a failed candidate must not touch a subscriber list.
        value->storage_ = nullptr;
        value->words_.resize(std::size_t(input.record_count) + 5);
        value->positions_.resize(input.record_count, UINT32_MAX);
        const auto& submesh = info->description.submeshes[input.group->submesh];
        value->words_[0] = submesh.index_count;
        value->words_[2] = submesh.first_index;
        value->words_[3] = std::bit_cast<std::uint32_t>(submesh.vertex_offset);
        for (const auto& entry : storage.entries_)
        {
            const MeshInstanceDrawGroup key{entry.mesh, entry.material, entry.submesh};
            if (key == *input.group)
            {
                if (entry.key.index >= input.record_count)
                {
                    return cxx::unexpected(RenderError{kStorageCapacity});
                }
                value->positions_[entry.key.index] = value->words_[1];
                value->words_[5 + value->words_[1]++] = entry.key.index;
            }
        }
        auto buffer = storage.session_.allocator().createBuffer(
            value->words_.size() * sizeof(std::uint32_t),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            EMemoryAccess::DEVICE
        );
        if (!buffer)
        {
            return cxx::unexpected(buffer.error());
        }
        value->buffer_ = std::move(*buffer);
        auto& uses = storage.draw_subscribers_[*input.group];
        value->position_ = uses.size();
        uses.push_back(value.get());
        value->storage_ = &storage;
        binding.buffer = value->buffer_.get();
        value->changed();
        binding.reference = {value.release(), [](void* pointer) noexcept { delete static_cast<MeshDraws*>(pointer); }};
        return {};
    }
} // namespace lux::render::vulkan
