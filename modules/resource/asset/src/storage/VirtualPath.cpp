#include <lux/engine/resource/asset/storage/VirtualPath.hpp>

namespace lux::asset
{
    namespace
    {
        // Validate one '/'-free segment. Empty -> EMPTY_SEGMENT; any byte
        // outside the frozen charset -> ILLEGAL_CHARACTER. ".."/"." need no
        // special case: '.' itself is an illegal name char.
        std::optional<EVirtualPathError> validateSegment(std::string_view seg) noexcept
        {
            if (seg.empty())
                return EVirtualPathError::EMPTY_SEGMENT;
            for (const char c : seg)
            {
                if (!VirtualPath::isLegalChar(c))
                    return EVirtualPathError::ILLEGAL_CHARACTER;
            }
            return std::nullopt;
        }
    }

    bool VirtualPath::isLegalChar(char c) noexcept
    {
        const auto b = static_cast<unsigned char>(c);
        if (b < 0x20 || b == 0x7F)   // control characters
            return false;
        if (b >= 0x80)               // UTF-8 continuation / lead bytes pass
            return true;
        switch (c)
        {
        case '/': case '\\': case '.': case ':':
        case '"': case '|':  case '?': case '*':
        case '<': case '>':
            return false;
        default:
            return true;
        }
    }

    bool VirtualPath::isLegalRoot(std::string_view root) noexcept
    {
        if (root.size() < 2 || root.front() != '/')
            return false;
        return !validateRelative(root.substr(1)).has_value()
            && root.find('/', 1) == std::string_view::npos;
    }

    std::optional<EVirtualPathError>
    VirtualPath::validateRelative(std::string_view rel) noexcept
    {
        if (rel.empty())
            return EVirtualPathError::EMPTY;
        if (rel.size() > kMaxLength)
            return EVirtualPathError::TOO_LONG;
        if (rel.front() == '/')
            return EVirtualPathError::NOT_ABSOLUTE; // relative form must NOT lead with '/'

        std::size_t pos = 0;
        while (true)
        {
            const std::size_t slash = rel.find('/', pos);
            const std::string_view seg =
                rel.substr(pos, slash == std::string_view::npos ? std::string_view::npos
                                                                : slash - pos);
            if (auto err = validateSegment(seg))
                return err;                       // covers "a//b" and trailing '/'
            if (slash == std::string_view::npos)
                return std::nullopt;
            pos = slash + 1;
        }
    }

    lux::cxx::expected<VirtualPath, EVirtualPathError>
    VirtualPath::parse(std::string_view text)
    {
        if (text.empty())
            return lux::cxx::unexpected(EVirtualPathError::EMPTY);
        if (text.size() > kMaxLength)
            return lux::cxx::unexpected(EVirtualPathError::TOO_LONG);
        if (text.front() != '/')
            return lux::cxx::unexpected(EVirtualPathError::NOT_ABSOLUTE);

        const std::string_view body = text.substr(1); // after the leading '/'

        // Root segment.
        const std::size_t root_end = body.find('/');
        const std::string_view root_seg =
            body.substr(0, root_end == std::string_view::npos ? std::string_view::npos
                                                              : root_end);
        if (auto err = validateSegment(root_seg))
            return lux::cxx::unexpected(*err);
        if (root_end == std::string_view::npos)
            return lux::cxx::unexpected(EVirtualPathError::MISSING_ASSET_NAME);

        // Remainder = mount-relative part; must itself be valid and non-empty.
        // An empty remainder ("/Game/") or one starting with '/' ("/Game//x")
        // is structurally an empty segment, not a missing/relative path.
        const std::string_view rel = body.substr(root_end + 1);
        if (rel.empty() || rel.front() == '/')
            return lux::cxx::unexpected(EVirtualPathError::EMPTY_SEGMENT);
        if (auto err = validateRelative(rel))
            return lux::cxx::unexpected(*err);

        VirtualPath out;
        out.path_     = std::string{ text };
        out.root_len_ = root_seg.size();
        const std::size_t last_slash = text.rfind('/');
        out.name_pos_ = last_slash + 1;
        return out;
    }

    std::string foldCaseAscii(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (const char c : s)
            out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
        return out;
    }
}
