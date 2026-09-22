#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <lux/engine/editor/detail/EditorImpl.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <thread>

namespace lux::editor
{

namespace
{
auto failed(EEditorError code)
{
    return lux::cxx::unexpected(EditorFailure{code, "editor"});
}

std::uint64_t issueEditorIdentity() noexcept
{
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value != UINT64_MAX)
    {
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
        {
            return value;
        }
    }
    return 0;
}

void report(const EditorFailure &error)
{
    std::fprintf(stderr, "%s:%llu %s\n", error.domain.c_str(), static_cast<unsigned long long>(error.reason),
                 error.message.c_str());
}

} // namespace

Editor::Editor(EditorConfig config) : config_(std::move(config)), identity_(issueEditorIdentity())
{
    requests_.reserve(config_.limits.open_requests);
    openings_.reserve(config_.limits.documents);
    documents_.reserve(config_.limits.documents);
}

Editor::~Editor() = default;

EditorResult<void> Editor::registerDocument(DocumentRegistration registration)
{
    if (state_ == EState::CLOSING || state_ == EState::FINISHED)
    {
        return failed(EEditorError::CLOSING);
    }
    if (registration.type.empty() || !registration.open)
    {
        return failed(EEditorError::INVALID_ARGUMENT);
    }
    if (std::ranges::find(registrations_, registration.type, &DocumentRegistration::type) != registrations_.end())
    {
        return failed(EEditorError::INVALID_ARGUMENT);
    }
    registrations_.push_back(std::move(registration));
    return {};
}

EditorResult<OpenRequestId> Editor::requestOpen(const OpenDocumentRequest &request)
{
    if (state_ != EState::RUNNING || !project_ || exit_requested_ || review_.serial)
    {
        return failed(EEditorError::CLOSING);
    }
    if (request.key.project != project_->manifest().id || request.key.source.isNull())
    {
        return failed(EEditorError::INVALID_ARGUMENT);
    }
    if (requests_.size() == config_.limits.open_requests || next_request_ == UINT64_MAX)
    {
        return failed(EEditorError::CAPACITY);
    }

    const OpenRequestId id{next_request_};
    for (const auto &document : documents_.values())
    {
        if (document->summary().key == request.key)
        {
            if (document->closeStatus().state != ECloseState::OPEN)
            {
                return failed(EEditorError::CLOSING);
            }
            requests_.push_back({id, document->handle()});
            ++next_request_;
            return id;
        }
    }
    for (const auto &opening : openings_)
    {
        if (opening->key == request.key)
        {
            if (opening->waiters.empty())
            {
                return failed(EEditorError::CLOSING);
            }
            opening->waiters.push_back(id);
            requests_.push_back({id});
            ++next_request_;
            return id;
        }
    }

    if (documents_.size() + openings_.size() >= config_.limits.documents)
    {
        return failed(EEditorError::CAPACITY);
    }
    const auto registration = std::ranges::find(registrations_, request.key.type, &DocumentRegistration::type);
    if (registration == registrations_.end())
    {
        return failed(EEditorError::MISSING_PROVIDER);
    }
    const auto factory = registration->open;
    auto opening = std::make_unique<Opening>(Opening{request.key, {}, {id}});
    auto *pending = opening.get();
    openings_.push_back(std::move(opening));
    requests_.push_back({id});
    ++next_request_;

    // The key is occupied before provider code can complete inline or reenter requestOpen.
    auto work = factory(*project_, request);
    if (work && *work)
    {
        pending->work = std::move(*work);
        if (pending->waiters.empty() || exit_requested_)
        {
            pending->work->cancel();
        }
    }
    else
    {
        const auto failure = work ? EditorFailure{EEditorError::INVALID_STATE, "document.factory"} : work.error();
        for (const auto waiter : pending->waiters)
        {
            std::ranges::find(requests_, waiter, &Request::id)->status = failure;
        }
        std::erase_if(openings_, [pending](const auto &value) { return value.get() == pending; });
    }
    return id;
}

EditorResult<void> Editor::cancelOpen(OpenRequestId id)
{
    const auto found = std::ranges::find(requests_, id, &Request::id);
    if (found == requests_.end())
    {
        return failed(EEditorError::STALE_REQUEST);
    }
    if (!std::holds_alternative<OpenPending>(found->status))
    {
        return failed(EEditorError::INVALID_STATE);
    }
    found->status = OpenCancelled{};
    for (const auto &opening : openings_)
    {
        const auto removed = std::erase(opening->waiters, id);
        if (removed && opening->waiters.empty() && opening->work)
        {
            opening->work->cancel();
        }
    }
    return {};
}

EditorResult<OpenRequestStatus> Editor::openStatus(OpenRequestId id) const
{
    const auto found = std::ranges::find(requests_, id, &Request::id);
    if (found == requests_.end())
    {
        return failed(EEditorError::STALE_REQUEST);
    }
    return found->status;
}

EditorResult<void> Editor::acknowledgeOpen(OpenRequestId id)
{
    const auto found = std::ranges::find(requests_, id, &Request::id);
    if (found == requests_.end())
    {
        return failed(EEditorError::STALE_REQUEST);
    }
    if (std::holds_alternative<OpenPending>(found->status))
    {
        return failed(EEditorError::BUSY);
    }
    requests_.erase(found);
    return {};
}

EditorResult<std::reference_wrapper<DocumentEditor>> Editor::document(DocumentHandle handle)
{
    const auto *value = documents_.find(handle);
    if (!value)
    {
        return failed(EEditorError::STALE_DOCUMENT);
    }
    return std::ref(**value);
}

std::vector<DocumentSummary> Editor::documents() const
{
    std::vector<DocumentSummary> result;
    result.reserve(documents_.size());
    for (const auto &document : documents_.values())
    {
        result.push_back(document->summary());
    }
    return result;
}

EditorResult<void> Editor::cancelStartup() noexcept
{
    if (project_ || state_ != EState::RUNNING || exit_requested_)
    {
        return failed(EEditorError::INVALID_STATE);
    }
    std::fprintf(stderr, "[editor.exit] event=startup-cancel\n");
    exit_requested_ = true;
    return {};
}

EditorResult<ExitReviewId> Editor::beginExitReview()
{
    if (state_ != EState::RUNNING || exit_requested_)
    {
        return failed(EEditorError::CLOSING);
    }
    if (review_.serial)
    {
        return failed(EEditorError::BUSY);
    }
    if (!identity_ || next_review_ == UINT64_MAX)
    {
        return failed(EEditorError::CAPACITY);
    }
    review_ = {identity_, next_review_++};
    std::fprintf(stderr, "[editor.exit] event=review-begin review=%llu\n",
                 static_cast<unsigned long long>(review_.serial));
    return review_;
}

EditorResult<void> Editor::cancelExitReview(ExitReviewId id)
{
    if (!id.serial || id != review_)
    {
        return failed(EEditorError::STALE_REQUEST);
    }
    if (exit_requested_)
    {
        return failed(EEditorError::CLOSING);
    }
    std::fprintf(stderr, "[editor.exit] event=review-cancel review=%llu\n", static_cast<unsigned long long>(id.serial));
    review_ = {};
    return {};
}

EditorResult<void> Editor::commitExitReview(ExitReviewId id, std::span<const DocumentCloseDecision> decisions)
{
    if (!id.serial || id != review_)
    {
        return failed(EEditorError::STALE_REQUEST);
    }
    if (exit_requested_ || state_ != EState::RUNNING)
    {
        return failed(EEditorError::CLOSING);
    }
    if (!openings_.empty())
    {
        return failed(EEditorError::BUSY);
    }
    if (decisions.size() != documents_.size())
    {
        return failed(EEditorError::STALE_DOCUMENT);
    }

    // No callbacks or close effects occur until every current owner has been reviewed.
    for (const auto &document : documents_.values())
    {
        const auto count = std::ranges::count(decisions, document->handle(), &DocumentCloseDecision::document);
        if (count != 1)
        {
            return failed(EEditorError::STALE_DOCUMENT);
        }
        const auto &decision = *std::ranges::find(decisions, document->handle(), &DocumentCloseDecision::document);
        const auto current = document->reviewClose();
        if (!current)
        {
            return lux::cxx::unexpected(current.error());
        }
        if (current->current != decision.state || current->revision != decision.revision)
        {
            return failed(EEditorError::STALE_REQUEST);
        }
        if (decision.decision != EDocumentCloseDecision::DISCARD_THIS_STATE &&
            (decision.decision != EDocumentCloseDecision::CLOSE_CLEAN || !current->clean))
        {
            return failed(EEditorError::INVALID_STATE);
        }
    }

    std::fprintf(stderr, "[editor.exit] event=review-commit review=%llu documents=%zu\n",
                 static_cast<unsigned long long>(id.serial), decisions.size());
    exit_requested_ = true;
    for (const auto &document : documents_.values())
    {
        document->requestClose();
    }
    return {};
}

void Editor::fail(EditorFailure failure)
{
    if (outcome_)
    {
        std::fprintf(stderr, "[editor.exit] event=failure domain=%s code=%u reason=%llu\n", failure.domain.c_str(),
                     static_cast<unsigned>(failure.code), static_cast<unsigned long long>(failure.reason));
        report(failure);
        outcome_ = lux::cxx::unexpected(std::move(failure));
    }
    exit_requested_ = true;
}

void Editor::acceptOpenings(PollBudget &budget)
{
    for (std::size_t index{}; index < openings_.size();)
    {
        auto &opening = *openings_[index];
        opening.work->poll(budget);
        if (!opening.work->settled())
        {
            ++index;
            continue;
        }
        auto result = opening.work->take();
        OpenRequestStatus status;
        if (result && *result)
        {
            auto *document = result->get();
            const auto handle = documents_.insert(std::move(*result));
            document->handle_ = handle;
            status = handle;
            if (opening.waiters.empty() || exit_requested_)
            {
                document->requestClose();
            }
        }
        else
        {
            const auto failure = result ? EditorFailure{EEditorError::INVALID_STATE, "document.take"} : result.error();
            status = failure;
            report(failure);
        }
        for (const auto waiter : opening.waiters)
        {
            const auto found = std::ranges::find(requests_, waiter, &Request::id);
            if (found != requests_.end())
            {
                found->status = status;
            }
        }
        openings_.erase(openings_.begin() + index);
    }
}

void Editor::advanceOwners(PollBudget &budget)
{
    const auto &values = documents_.values();
    // UI publication and runtime retirement share the same finite budget as
    // documents. Rotate all owners, including an empty desktop, so none can
    // permanently consume the last call or Program slot ahead of another.
    const auto count = values.size() + 2;
    poll_cursor_ %= count;
    for (std::size_t index{}; index < count; ++index)
    {
        const auto owner = (poll_cursor_ + index) % count;
        if (owner < values.size())
        {
            values[owner]->poll(budget);
        }
        else if (owner == values.size())
        {
            advanceUi(budget);
        }
        else
        {
            pumpRender(budget);
        }
    }
    poll_cursor_ = (poll_cursor_ + 1) % count;
}

void Editor::collectClosed()
{
    std::vector<DocumentHandle> retired;
    for (const auto &document : documents_.values())
    {
        const auto closed = document->closeStatus();
        if (closed.state == ECloseState::CLOSED)
        {
            if (!closed.progress)
            {
                fail(closed.progress.error());
            }
            retired.push_back(document->handle());
        }
    }
    for (const auto handle : retired)
    {
        documents_.erase(handle);
    }
}

} // namespace lux::editor
