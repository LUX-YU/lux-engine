#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <exec/task.hpp>
#include <lux/engine/render/visibility.h>
#include <cstdint>
#include <span>

namespace lux::render::detail
{
    struct BufferWrite;
    class DeviceEntry;

    struct ReplicaFrame final
    {
        std::uint32_t slot;
        std::uint64_t epoch;
    };
}

namespace lux::render
{
    // Borrowed by one admitted Resource command on the device owner. Validate CPU input before selecting
    // a writable replica. Await all writes before returning; the command owns selection and settlement.
    class LUX_RENDER_PUBLIC ReplicaMutation final
    {
    public:
        [[nodiscard]] exec::task<RenderResult<detail::ReplicaFrame>> select() noexcept;
        [[nodiscard]] exec::task<RenderResult<void>> writeBuffers(std::span<const detail::BufferWrite>) noexcept;

        ReplicaMutation(const ReplicaMutation&) = delete;
        ReplicaMutation& operator=(const ReplicaMutation&) = delete;

    private:
        friend class CommandBinding;
        explicit ReplicaMutation(detail::DeviceEntry& device) noexcept : device_(device) {}
        ~ReplicaMutation() noexcept;

        detail::DeviceEntry& device_;
        detail::ReplicaFrame selected_{};
    };
}
