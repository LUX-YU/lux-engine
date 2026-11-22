#include <lux/engine/render/resource/ProgramInputRevision.hpp>
#include <exception>

namespace lux::render
{
    ProgramInputRevision::~ProgramInputRevision() noexcept
    {
        if (first_) std::terminate();
    }

    void ProgramInputRevision::advance() noexcept
    {
        if (value_ == UINT64_MAX) std::terminate();
        ++value_;
        for (const auto* use = first_; use; use = use->next)
        {
            if (*use->execution_revision == UINT64_MAX) std::terminate();
            ++*use->execution_revision;
            for (auto* dirty : use->dirty_programs) *dirty = true;
        }
    }

    void ProgramInputRevision::attach(Use& use) noexcept
    {
        use.next = first_;
        if (first_) first_->previous = &use;
        first_ = &use;
    }

    void ProgramInputRevision::detach(Use& use) noexcept
    {
        if (use.previous) use.previous->next = use.next;
        else first_ = use.next;
        if (use.next) use.next->previous = use.previous;
        use.next = use.previous = nullptr;
    }
} // namespace lux::render
