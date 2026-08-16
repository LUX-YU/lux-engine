#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace lux::ecs::detail
{
    /// Private slot-lifetime cache shared by the sparse Pixel and Tilemap
    /// render bridges. A retired slot remains unavailable until every upload
    /// issued for its previous occupant has settled, preventing late writes
    /// from corrupting a replacement occupant.
    ///
    /// This is deliberately a composition object rather than a public canvas
    /// base class: content discovery, extraction, and retry policy remain
    /// owned by each domain.
    class SparseCanvasAtlasCache final
    {
    public:
        explicit SparseCanvasAtlasCache(std::uint32_t slot_count)
            : slots_(slot_count)
        {
            free_slots_.reserve(slot_count);
            for (std::uint32_t slot = slot_count; slot > 0u; --slot)
                free_slots_.push_back(slot - 1u);
        }

        [[nodiscard]] std::optional<std::uint32_t> acquire() noexcept
        {
            while (!free_slots_.empty())
            {
                const auto slot = free_slots_.back();
                free_slots_.pop_back();
                if (slot >= slots_.size() || slots_[slot].owned)
                {
                    ++protocol_errors_;
                    continue;
                }

                auto& state = slots_[slot];
                state.owned = true;
                state.retired = false;
                state.uploads = 0u;
                return slot;
            }
            return std::nullopt;
        }

        /// Return a slot for which no render upload was issued. Create
        /// rejection and create-reply failure use this path.
        [[nodiscard]] bool releaseUnpublished(std::uint32_t slot) noexcept
        {
            if (!validOwned(slot))
                return reject();
            auto& state = slots_[slot];
            if (state.retired || state.uploads != 0u)
                return reject();
            release(slot, state);
            return true;
        }

        /// Fence a newly accepted upload against reuse of its destination.
        [[nodiscard]] bool beginUpload(std::uint32_t slot) noexcept
        {
            if (!validOwned(slot))
                return reject();
            auto& state = slots_[slot];
            if (state.retired ||
                state.uploads == std::numeric_limits<std::uint32_t>::max())
            {
                return reject();
            }
            ++state.uploads;
            return true;
        }

        /// Retire an occupant. Reuse is immediate only when no upload is in
        /// flight; otherwise finishUpload performs the eventual release.
        [[nodiscard]] bool retire(std::uint32_t slot) noexcept
        {
            if (!validOwned(slot))
                return reject();
            auto& state = slots_[slot];
            if (state.retired)
                return reject();
            if (state.uploads == 0u)
                release(slot, state);
            else
                state.retired = true;
            return true;
        }

        [[nodiscard]] bool finishUpload(std::uint32_t slot) noexcept
        {
            if (!validOwned(slot))
                return reject();
            auto& state = slots_[slot];
            if (state.uploads == 0u)
                return reject();
            --state.uploads;
            if (state.uploads == 0u && state.retired)
                release(slot, state);
            return true;
        }

        [[nodiscard]] std::uint32_t freeSlots() const noexcept
        {
            return static_cast<std::uint32_t>(free_slots_.size());
        }

        [[nodiscard]] std::uint64_t protocolErrors() const noexcept
        {
            return protocol_errors_;
        }

    private:
        struct SlotState final
        {
            std::uint32_t uploads{0u};
            bool owned{false};
            bool retired{false};
        };

        [[nodiscard]] bool validOwned(std::uint32_t slot) const noexcept
        {
            return slot < slots_.size() && slots_[slot].owned;
        }

        [[nodiscard]] bool reject() noexcept
        {
            ++protocol_errors_;
            return false;
        }

        void release(std::uint32_t slot, SlotState& state) noexcept
        {
            state = {};
            free_slots_.push_back(slot);
        }

        std::vector<SlotState> slots_;
        std::vector<std::uint32_t> free_slots_;
        std::uint64_t protocol_errors_{0u};
    };
}
