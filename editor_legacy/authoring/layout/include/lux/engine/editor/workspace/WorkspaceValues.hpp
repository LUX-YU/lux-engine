#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/editor/views/ViewContent.hpp>
#include <optional>
#include <span>
#include <vector>

namespace lux::editor::workspace
{
    struct LayoutId final
    {
        std::string value;
        [[nodiscard]] bool valid() const noexcept;
        friend bool operator==(const LayoutId&, const LayoutId&) = default;
    };
    struct LayoutSlotId final
    {
        std::uint32_t value{};
        friend bool operator==(LayoutSlotId, LayoutSlotId) = default;
    };
    enum class EWorkspaceError : std::uint8_t
    {
        INVALID_DATA,
        UNSUPPORTED_VERSION,
        CAPACITY,
        NOT_FOUND,
        IO,
        CONFLICT,
        BUSY
    };
    struct WorkspaceFailure final
    {
        EWorkspaceError code{EWorkspaceError::INVALID_DATA};
        std::string detail;
        std::uint64_t native_code{};
    };
    template <class T> using WorkspaceResult = lux::cxx::expected<T, WorkspaceFailure>;
    struct WorkspaceLimits final
    {
        std::size_t file_bytes{16 * 1024 * 1024};
        std::size_t opaque_bytes{8 * 1024 * 1024};
        std::size_t entries{4096};
        std::size_t depth{64};
        std::size_t text_bytes{4096};
    };
    struct PreservedOpaqueState final
    {
        std::string type;
        std::uint32_t schema{1};
        std::vector<std::byte> bytes;
        friend bool operator==(const PreservedOpaqueState&, const PreservedOpaqueState&) = default;
    };
    struct VersionedViewState final
    {
        std::uint32_t schema{1};
        std::vector<std::byte> bytes;
        friend bool operator==(const VersionedViewState&, const VersionedViewState&) = default;
    };
    // A persistent provenance proof, not an in-flight migration flag.
    struct LegacyOrigin final
    {
        std::string key;
        std::string digest;
        friend bool operator==(const LegacyOrigin&, const LegacyOrigin&) = default;
    };
    struct UserPreferences final
    {
        std::uint32_t schema{1};
        std::optional<LayoutId> selected_layout;
        std::vector<PreservedOpaqueState> opaque;
        std::optional<LegacyOrigin> legacy_origin;
    };
    [[nodiscard]] WorkspaceResult<void> validatePreferences(const UserPreferences&, WorkspaceLimits = {});
    [[nodiscard]] WorkspaceResult<std::vector<std::byte>> encodePreferences(
        const UserPreferences&,
        WorkspaceLimits = {}
    );
    [[nodiscard]] WorkspaceResult<UserPreferences> decodePreferences(std::span<const std::byte>, WorkspaceLimits = {});
}
