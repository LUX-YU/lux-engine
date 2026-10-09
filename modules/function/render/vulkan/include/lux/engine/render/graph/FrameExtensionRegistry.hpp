#pragma once

#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/container/HeterogeneousLookup.hpp>
#include <lux/engine/function/visibility.h>
#include <string_view>

namespace lux::render
{
    using FrameExtensionSlotId = std::uint8_t;
    inline constexpr FrameExtensionSlotId kInvalidExtSlot = 0;
    inline constexpr std::uint32_t kMaxFrameExtensionSlots = 32;

    enum class EFrameExtensionRegistrationError
    {
        INVALID_NAME,
        CAPACITY
    };
    using FrameExtensionResult = lux::cxx::expected<FrameExtensionSlotId, EFrameExtensionRegistrationError>;

    /// The only frame-extension allocator. Register during serialized render composition,
    /// before compiling/executing graphs. Names and slots remain valid until process shutdown.
    class LUX_FUNCTION_PUBLIC FrameExtensionRegistry final
    {
    public:
        static FrameExtensionRegistry& instance() noexcept;
        FrameExtensionRegistry(const FrameExtensionRegistry&) = delete;
        FrameExtensionRegistry& operator=(const FrameExtensionRegistry&) = delete;

        [[nodiscard]] FrameExtensionResult registerSlot(std::string_view canonical_name) noexcept;
        [[nodiscard]] FrameExtensionSlotId idOf(std::string_view canonical_name) const noexcept;

    private:
        FrameExtensionRegistry() = default;
        lux::cxx::heterogeneous_map<FrameExtensionSlotId> name_to_id_;
    };
} // namespace lux::render
