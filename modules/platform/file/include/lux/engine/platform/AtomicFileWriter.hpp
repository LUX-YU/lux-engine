#pragma once

#include <lux/engine/platform/FileIo.hpp>

namespace lux::platform
{
    /// Owns one exclusively created temporary file beside the absolute destination.
    /// Destruction removes an unpublished candidate; commit is the only publication point.
    class AtomicFileWriter final
    {
    public:
        [[nodiscard]] static FileResult<AtomicFileWriter> create(
            const std::filesystem::path& destination,
            EFileWriteMode mode
        ) noexcept;

        AtomicFileWriter(const AtomicFileWriter&) = delete;
        AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;
        AtomicFileWriter(AtomicFileWriter&& other) noexcept;
        AtomicFileWriter& operator=(AtomicFileWriter&& other) noexcept;
        ~AtomicFileWriter() noexcept;

        /// Write the entire range, handling short writes. Cancellation leaves the candidate unpublished.
        [[nodiscard]] FileResult<void> write(std::span<const std::byte> bytes, std::stop_token stop = {}) noexcept;

        /// Absolute byte position, for streaming formats that backfill a header.
        [[nodiscard]] FileResult<void> seek(std::uint64_t offset) noexcept;

        /// Flush, close and publish once. No cancellation is reported after publication.
        /// PUBLICATION_UNKNOWN means visibility/durability may have changed; never blindly retry.
        [[nodiscard]] FileResult<void> commit(std::stop_token stop = {}) noexcept;

    private:
        AtomicFileWriter() noexcept = default;
        void close() noexcept;
        void discard() noexcept;

        std::filesystem::path destination_;
        std::filesystem::path temporary_;
        std::intptr_t handle_{-1};
        EFileWriteMode mode_{};
        bool owns_temporary_{};
    };
} // namespace lux::platform
