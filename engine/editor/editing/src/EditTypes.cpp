#include <lux/engine/editor/editing/EditHistoryTarget.hpp>
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
        result.message_truncated = copied < message.size();
        return result;
    }

    PreparedEdit::~PreparedEdit() noexcept = default;
    EditOperation::~EditOperation() noexcept = default;
    EditHistoryTarget::~EditHistoryTarget() noexcept = default;

    EditPreparationBudget::EditPreparationBudget(std::size_t limit) noexcept : limit_(limit)
    {
    }
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

    SaveTicket::SaveTicket(HistoryId history, StateId state, std::uint64_t request) noexcept
        : history_(history), state_(state), request_(request)
    {
    }
    bool SaveTicket::valid() const noexcept
    {
        return history_.valid() && state_.valid() && state_.history == history_ && request_ != 0U;
    }
    HistoryId SaveTicket::history() const noexcept
    {
        return history_;
    }
    StateId SaveTicket::state() const noexcept
    {
        return state_;
    }
    std::uint64_t SaveTicket::request() const noexcept
    {
        return request_;
    }
} // namespace lux::editor::editing
