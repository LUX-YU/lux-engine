#include <lux/engine/serialize/NameTable.hpp>

#include <lux/engine/serialize/Archive.hpp>

#include <cassert>

namespace lux::serialize
{

    NameTable::NameTable()
    {
        // Index 0 is reserved for the empty string ("no name" sentinel).
        // We store it explicitly so `at(0)` returns a stable string_view.
        strings_.emplace_back();
        // Don't insert into index_ — empty string is the implicit fallback.
    }

    std::uint32_t NameTable::intern(std::string_view s)
    {
        if (s.empty())
            return 0u;

        if (auto it = index_.find(s); it != index_.end())
            return it->second;

        const auto next_idx = static_cast<std::uint32_t>(strings_.size());
        strings_.emplace_back(s);
        // Rehash-safe: index_ stores string_views pointing into the
        // freshly-emplaced storage. std::string grows but the small-string
        // buffer or heap allocation owns its bytes for the lifetime of the
        // element, so the view stays valid as long as we never erase or
        // reorder strings_ — which we never do.
        index_.emplace(std::string_view(strings_.back()), next_idx);
        return next_idx;
    }

    std::string_view NameTable::at(std::uint32_t idx) const noexcept
    {
        if (idx >= strings_.size())
        {
            assert(false && "NameTable::at: index out of range");
            return {};
        }
        return strings_[idx];
    }

    void NameTable::serialize(ArchiveWriter& ar) const
    {
        // count includes the implicit index-0 empty entry. Decoder reads
        // `count - 1` actual strings and assumes index 0 is empty.
        const auto count = static_cast<std::uint32_t>(strings_.size());
        ar.writePod(count);
        for (std::uint32_t i = 1; i < count; ++i)
            ar.writeString(strings_[i]);
    }

    NameTable NameTable::deserialize(ArchiveReader& ar)
    {
        NameTable nt;
        const auto count = ar.readPod<std::uint32_t>();
        if (count == 0)
            return nt; // empty file — index 0 already present
        // The first entry on disk is at logical index 1; index 0 is the
        // implicit empty sentinel populated by the ctor.
        nt.strings_.reserve(count);
        for (std::uint32_t i = 1; i < count; ++i)
        {
            std::string s = ar.readString();
            nt.strings_.emplace_back(std::move(s));
            nt.index_.emplace(std::string_view(nt.strings_.back()),
                              static_cast<std::uint32_t>(nt.strings_.size() - 1));
        }
        return nt;
    }

} // namespace lux::serialize
