#pragma once

#include <lux/engine/render/visibility.h>
#include <cstdint>

namespace lux::render
{
    namespace detail { class RasterProgramInputs; }

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
        friend class detail::RasterProgramInputs;
        struct Use;

        void attach(Use&) noexcept;
        void detach(Use&) noexcept;

        std::uint64_t value_ = 1;
        Use* first_ = nullptr;
    };
} // namespace lux::render
