#include <lux/engine/editor/editing/EditOperation.hpp>

#include <algorithm>
#include <cstring>

namespace lux::editor::editing
{
    EditFailure makeEditFailure(EEditError code, std::uint64_t domain_code, std::string_view message) noexcept
    {
        EditFailure result{};
        result.code = code;
        result.domain_code = domain_code;
        const auto nul = message.find('\0');
        const auto length = (std::min)(message.size(), nul);
        const auto copied = (std::min)(length, result.message.size() - 1U);
        if (copied != 0U)
        {
            std::memcpy(result.message.data(), message.data(), copied);
        }
        const bool has_suffix = nul != std::string_view::npos && nul + 1U < message.size();
        result.message_truncated = copied < length || has_suffix;
        return result;
    }

    PreparedEdit::~PreparedEdit() noexcept = default;
    EditOperation::~EditOperation() noexcept = default;

    EditPreparationBudget::EditPreparationBudget(std::size_t limit) noexcept : limit_(limit) {}
    EditResult<void> EditPreparationBudget::reserve(std::size_t bytes) noexcept
    {
        if (bytes > remaining())
        {
            return lux::cxx::unexpected(makeEditFailure(EEditError::STAGING_LIMIT));
        }
        used_ += bytes;
        return {};
    }
    std::size_t EditPreparationBudget::limit() const noexcept
    {
        return limit_;
    }
    std::size_t EditPreparationBudget::used() const noexcept
    {
        return used_;
    }
    std::size_t EditPreparationBudget::remaining() const noexcept
    {
        return limit_ - used_;
    }

} // namespace lux::editor::editing
