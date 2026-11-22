#pragma once

#include <lux/engine/render/visibility.h>
#include <cstdint>
#include <vector>

namespace lux::render
{
    namespace detail { class RenderGraphInstance; }

    // Resource-owned identity of a program snapshot. Access and publication run on the device owner.
    class LUX_RENDER_PUBLIC ProgramInputRevision final
    {
    public:
        ProgramInputRevision() noexcept = default;
        ~ProgramInputRevision() noexcept;
        ProgramInputRevision(const ProgramInputRevision&) = delete;
        ProgramInputRevision& operator=(const ProgramInputRevision&) = delete;

        [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
        void advance() noexcept;

    private:
        friend class detail::RenderGraphInstance;

        // An installed execution owns this row and its program flags until it detaches.
        struct Use final
        {
            ProgramInputRevision* source;
            std::uint64_t* execution_revision;
            std::vector<bool*> dirty_programs;
            Use* previous = nullptr;
            Use* next = nullptr;
        };

        void attach(Use&) noexcept;
        void detach(Use&) noexcept;

        std::uint64_t value_ = 1;
        Use* first_ = nullptr;
    };
} // namespace lux::render
