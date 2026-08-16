#pragma once
/**
 * @file SlotMetaVector.hpp
 * @brief Sparse metadata array indexed by SlotKey-based handles.
 *
 * Provides O(1) constant-time access by using handle.index as a direct
 * array offset.  Grows automatically when new handles are inserted.
 * No hashing overhead — just a plain vector<T> + generation validation.
 */
#include <vector>
#include <cstdint>
#include <optional>
#include <cassert>

namespace lux::render
{
    /**
     * @tparam Meta    Payload type (must be default-constructible).
     * @tparam Handle  SlotKey-like handle type (needs .index, .gen, .isValid()).
     */
    template <typename Meta, typename Handle>
    class SlotMetaVector
    {
    public:
        SlotMetaVector() = default;

        /// Store metadata for a handle. Grows the internal vectors if needed.
        void insert(Handle h, const Meta& value)
        {
            ensureCapacity(h.index + 1);
            data_[h.index] = value;
            gens_[h.index] = h.gen;
            alive_[h.index] = true;
        }

        /// Erase metadata for a handle. Returns true if found and erased.
        bool erase(Handle h)
        {
            if (!contains(h)) return false;
            alive_[h.index] = false;
            data_[h.index] = Meta{};
            return true;
        }

        /// Check whether a handle has live metadata.
        [[nodiscard]] bool contains(Handle h) const noexcept
        {
            return h.isValid()
                && h.index < data_.size()
                && alive_[h.index]
                && gens_[h.index] == h.gen;
        }

        /// Access metadata. Returns nullptr if handle is stale or absent.
        [[nodiscard]] Meta* find(Handle h) noexcept
        {
            return contains(h) ? &data_[h.index] : nullptr;
        }

        [[nodiscard]] const Meta* find(Handle h) const noexcept
        {
            return contains(h) ? &data_[h.index] : nullptr;
        }

        /// Access metadata, returning std::nullopt if not found.
        [[nodiscard]] std::optional<Meta> get(Handle h) const
        {
            if (!contains(h)) return std::nullopt;
            return data_[h.index];
        }

        /// Direct mutable reference (UB if !contains(h) — use after contains() check).
        [[nodiscard]] Meta& operator[](Handle h)
        {
            assert(contains(h));
            return data_[h.index];
        }

        [[nodiscard]] const Meta& operator[](Handle h) const
        {
            assert(contains(h));
            return data_[h.index];
        }

        void clear()
        {
            data_.clear();
            gens_.clear();
            alive_.clear();
        }

        [[nodiscard]] std::size_t capacity() const noexcept { return data_.size(); }

    private:
        void ensureCapacity(uint32_t required)
        {
            if (required > data_.size())
            {
                data_.resize(required);
                gens_.resize(required, 0);
                alive_.resize(required, false);
            }
        }

        std::vector<Meta>     data_;
        std::vector<uint32_t> gens_;
        std::vector<bool>     alive_;
    };

} // namespace lux::render
