#pragma once

#include <cstdint>

namespace lux::render
{
    class VertexPoolRegistry;
    class IVertexSource;

    namespace detail
    {
        // The move-only lease owns this fixed-address record. Both endpoint indexes borrow it.
        // Teardown clears both indexes before either endpoint address can be reused.
        struct VertexRegistration final
        {
            VertexRegistration(VertexPoolRegistry& registry, IVertexSource& source, std::uint32_t slot) noexcept;
            ~VertexRegistration() noexcept;
            VertexRegistration(const VertexRegistration&) = delete;
            VertexRegistration& operator=(const VertexRegistration&) = delete;
            VertexRegistration(VertexRegistration&&) = delete;
            VertexRegistration& operator=(VertexRegistration&&) = delete;

            void revoke(bool source_destroying = false) noexcept;

            VertexPoolRegistry* registry;
            IVertexSource* source;
            const std::uint32_t slot;
        };
    } // namespace detail
} // namespace lux::render
