#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/resource/ResourceIdentity.hpp>
#include <lux/engine/render/resource/transform/TransformSlotRetirement.hpp>
#include <lux/engine/render/resource/transform/Data.hpp>

namespace lux::render::detail
{
    // One local index addresses both the generation metadata and the shader's matrix row.
    class TransformRecords final
    {
    public:
        explicit TransformRecords(ResourceInstanceId owner) noexcept : owner_(owner) {}

        [[nodiscard]] ResourceInstanceId owner() const noexcept { return owner_; }
        [[nodiscard]] std::span<const TransformMatrix> matrices() const & noexcept { return matrices_; }
        std::span<const TransformMatrix> matrices() const && = delete;

        [[nodiscard]] bool contains(TransformId id) const noexcept
        {
            const auto key = Identity::local(id);
            if (Identity::parent(id) != owner_ || key.index >= entries_.size()) return false;
            const auto& entry = entries_[key.index];
            return entry.occupied && entry.generation == key.gen;
        }

        [[nodiscard]] bool canInsert(std::size_t count) const noexcept
        {
            const auto ready = reusable_.size();
            return count <= ready || count - ready <= UINT32_MAX - entries_.size();
        }

        // The batch caller preflights capacity and all matrix values before insertion.
        [[nodiscard]] TransformId insert(const TransformMatrix& matrix) noexcept
        {
            std::uint32_t index;
            if (reusable_.empty())
            {
                index = static_cast<std::uint32_t>(entries_.size());
                entries_.emplace_back();
            }
            else
            {
                index = reusable_.back();
                reusable_.pop_back();
            }
            auto& entry = entries_[index];
            entry.occupied = true;
            if (index == matrices_.size()) matrices_.push_back(matrix);
            else matrices_[index] = matrix;
            return Identity::make<TransformId>(owner_, cxx::SlotKey<TransformTag>{index, entry.generation});
        }

        void writeValidated(std::uint32_t index, const TransformMatrix& matrix) noexcept { matrices_[index] = matrix; }

        void retainInstance(TransformId id) noexcept
        {
            if (!contains(id)) std::terminate();
            auto& count = entries_[Identity::local(id).index].references;
            if (count == UINT32_MAX) std::terminate();
            ++count;
        }

        void releaseInstance(TransformId id) noexcept
        {
            if (!contains(id)) std::terminate();
            auto& count = entries_[Identity::local(id).index].references;
            if (!count) std::terminate();
            --count;
        }

        [[nodiscard]] RenderResult<void> release(TransformId id) noexcept
        {
            if (!contains(id)) return cxx::unexpected(RenderError{kStorageIdentity});
            const auto index = Identity::local(id).index;
            auto& entry = entries_[index];
            if (entry.references) return cxx::unexpected(RenderError{kDeviceBusy});
            entry.occupied = false;
            // A generation reaching the sentinel permanently retires this index.
            if (++entry.generation != UINT32_MAX) retirement_.release(index);
            return {};
        }

        [[nodiscard]] bool hasRetired() const noexcept { return !retirement_.empty(); }

        void collect(
            bool round_active, std::span<const SubmissionFrontiers> submitted, const SubmissionFrontiers& completed
        ) noexcept
        {
            retirement_.collect(round_active, submitted, completed, reusable_);
        }

    private:
        using Identity = extension_detail::ResourceIdentity;
        struct Entry final
        {
            std::uint32_t generation = 1, references = 0;
            bool occupied = false;
        };

        ResourceInstanceId owner_;
        std::vector<Entry> entries_;
        std::vector<TransformMatrix> matrices_{2};
        std::vector<std::uint32_t> reusable_;
        TransformSlotRetirement retirement_;
    };
}
