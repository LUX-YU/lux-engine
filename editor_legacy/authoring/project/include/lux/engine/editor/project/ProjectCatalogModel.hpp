#pragma once

#include <lux/engine/editor/project/AssetCatalog.hpp>
#include <lux/engine/editor/project/visibility.h>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <utility>

namespace lux::editor::project
{
    enum class EProjectQueryError : std::uint8_t
    {
        UNBOUND,
        BUSY,
        CLOSED,
        PERMISSION,
        IO,
        INVALID_PAYLOAD,
        CAPACITY
    };
    using VProjectQueryFailure = std::variant<EProjectQueryError, EAssetReferenceError>;
    template <class T> using ProjectQueryResult = lux::cxx::expected<T, VProjectQueryFailure>;

    struct ProjectCatalogVersion final
    {
        std::uint64_t instance{}, revision{};
        friend bool operator==(ProjectCatalogVersion, ProjectCatalogVersion) = default;
    };

    // Immutable owning catalog version. Copies share rows and their index; moved-from snapshots are empty.
    class LUX_EDITOR_PROJECT_PUBLIC ProjectCatalogSnapshot final
    {
    public:
        ProjectCatalogSnapshot() = default;
        ProjectCatalogSnapshot(const ProjectCatalogSnapshot&) = default;
        ProjectCatalogSnapshot& operator=(const ProjectCatalogSnapshot&) = default;
        ProjectCatalogSnapshot(ProjectCatalogSnapshot&&) noexcept = default;
        ProjectCatalogSnapshot& operator=(ProjectCatalogSnapshot&&) noexcept = default;
        [[nodiscard]] ProjectCatalogVersion version() const noexcept;
        [[nodiscard]] std::string_view name() const noexcept;
        [[nodiscard]] std::span<const AssetCatalogEntry> assets() const noexcept;
        // The result is borrowed from this frozen version, not the mutable Model's latest version.
        [[nodiscard]] const AssetCatalogEntry* find(asset::AssetId) const noexcept;
        [[nodiscard]] AssetReference reference(asset::AssetId id) const noexcept
        {
            const auto current = version();
            return {current.instance, current.revision, id};
        }

    private:
        friend class ProjectCatalogModel;
        struct Data;
        explicit ProjectCatalogSnapshot(std::shared_ptr<const Data>) noexcept;
        std::shared_ptr<const Data> owner_;
    };

    class LUX_EDITOR_PROJECT_PUBLIC ProjectCatalogModel final : public object::LuxObject
    {
    public:
        object::TSignal<std::uint64_t> changed{*this};
        ProjectCatalogModel(object::ObjectDispatcherRef, std::uint64_t instance);
        ~ProjectCatalogModel() override;
        ProjectCatalogModel(const ProjectCatalogModel&) = delete;
        ProjectCatalogModel& operator=(const ProjectCatalogModel&) = delete;
        ProjectCatalogModel(ProjectCatalogModel&&) = delete;
        ProjectCatalogModel& operator=(ProjectCatalogModel&&) = delete;
        [[nodiscard]] ProjectQueryResult<ProjectCatalogVersion> version() const noexcept;
        [[nodiscard]] ProjectQueryResult<ProjectCatalogSnapshot> snapshot() const noexcept;
        [[nodiscard]] ProjectQueryResult<asset::AssetId> resolve(AssetReference, std::uint32_t magic) const noexcept;
        [[nodiscard]] const AssetCatalogEntry* find(asset::AssetId) const noexcept;
        [[nodiscard]] AssetReference reference(asset::AssetId) const noexcept;
        [[nodiscard]] std::span<const AssetCatalogEntry> entries() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;
        // Prepare the whole replacement before publishing. Failure leaves the previous snapshot intact.
        [[nodiscard]] ProjectQueryResult<void> replace(std::string name, std::vector<AssetCatalogEntry>);
        void setFailure(std::optional<EProjectQueryError>) noexcept;
        [[nodiscard]] object::SignalDelivery dispatchChanges() noexcept;

    private:
        std::shared_ptr<const ProjectCatalogSnapshot::Data> snapshot_;
        std::optional<EProjectQueryError> failure_;
        bool notification_pending_{};
    };
    inline constexpr char kAssetReferencePayload[] = "lux.editor.asset-reference.v2";
    [[nodiscard]] LUX_EDITOR_PROJECT_PUBLIC ProjectQueryResult<
        AssetReference> decodeAssetReference(std::span<const std::byte>);
}
