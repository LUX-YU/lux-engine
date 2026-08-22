#include <lux/engine/core/serialization/NameTable.hpp>

#include <lux/engine/core/serialization/Archive.hpp>

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
        // index_ stores views into address-stable deque elements. This matters
        // for short names whose bytes live inside std::string's SSO buffer.
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
        if (!ar.ok())
            return nt;
        if (count == 0)
        {
            ar.invalidate();
            return nt;
        }
        if (count - 1u > ar.remaining() / sizeof(std::uint32_t))
        {
            ar.invalidate();
            return nt;
        }
        // The first entry on disk is at logical index 1; index 0 is the
        // implicit empty sentinel populated by the ctor.
        for (std::uint32_t i = 1; i < count; ++i)
        {
            std::string s = ar.readString();
            if (!ar.ok())
                return nt;
            nt.strings_.emplace_back(std::move(s));
            nt.index_.emplace(std::string_view(nt.strings_.back()),
                              static_cast<std::uint32_t>(nt.strings_.size() - 1));
        }
        return nt;
    }

} // namespace lux::serialize
