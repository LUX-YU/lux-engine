#pragma once

#include <lux/engine/editor/scene/SceneSource.hpp>
#include <lux/engine/editor/scene/SceneObjectRef.hpp>
#include <lux/engine/editor/sessions/SessionState.hpp>

namespace lux::editor::sessions
{
    class EditGate;
}

namespace lux::editor::scene
{
    struct SceneChangeCursor final
    {
        sessions::SessionId session;
        editing::HistoryId history;
        sessions::ObservationVersion observed;
        friend bool operator==(SceneChangeCursor, SceneChangeCursor) noexcept = default;
    };

    enum class ESceneChanges : std::uint8_t
    {
        DELTA,
        RESET_REQUIRED
    };

    struct SceneChangeSet final
    {
        ESceneChanges status{ESceneChanges::DELTA};
        SceneChangeCursor cursor;
        std::vector<world::WorldObjectId> objects;
        bool structure{};
        bool configuration{};
    };

    // Owns encoded values, including unknown data; never retains ComponentCapture's mutable value graph.
    class SceneSnapshot final
    {
    public:
        [[nodiscard]] sessions::ContentStamp content() const noexcept
        {
            return content_;
        }
        [[nodiscard]] SceneChangeCursor cursor() const noexcept
        {
            return cursor_;
        }
        [[nodiscard]] const SceneConfiguration& configuration() const noexcept
        {
            return configuration_;
        }
        [[nodiscard]] std::span<const SceneObjectData> objects() const noexcept
        {
            return objects_;
        }
        [[nodiscard]] std::span<const world::WorldPartitionId> partitionIds() const noexcept
        {
            return partition_ids_;
        }
        [[nodiscard]] std::span<const lux::cxx::SharedBytes<>> volumes() const noexcept
        {
            return volumes_;
        }
        [[nodiscard]] const lux::asset::PakDecodedImage& package() const noexcept
        {
            return package_;
        }
        [[nodiscard]] std::size_t retainedBytes() const noexcept
        {
            return retained_bytes_;
        }

    private:
        friend class SceneSession;
        friend struct detail::SceneSourceAccess;
        simulation::ecs::ComponentSchemaSet schemas_;
        sessions::ContentStamp content_;
        SceneChangeCursor cursor_;
        SceneConfiguration configuration_;
        std::vector<SceneObjectData> objects_;
        std::vector<world::WorldPartitionId> partition_ids_;
        std::vector<lux::cxx::SharedBytes<>> volumes_;
        lux::asset::PakDecodedImage package_;
        std::size_t retained_bytes_{};
    };

    // Shared frozen-content assembly for persistence and execution; no Registry/live Session borrow.
    [[nodiscard]] lux::cxx::expected<lux::scene::ScenePackage, lux::scene::ScenePackageFailure>
    buildSceneSnapshotPackage(const SceneSnapshot&, std::stop_token = {});

    // Synchronous owner-thread borrow, invalidated by any mutation/reload/close. Values read through
    // component() are owning encoded data; even plugin shared_ptr fields cannot mutate the source.
    class SceneReadView final
    {
    public:
        template <class Fn>
        [[nodiscard]] auto withRead(Fn&& function) const -> std::invoke_result_t<Fn, const SceneReadView&>
        {
            return gate_.withRead([&] { return std::invoke(std::forward<Fn>(function), *this); });
        }
        [[nodiscard]] std::vector<SceneObjectRef> objects() const;
        [[nodiscard]] bool contains(SceneObjectRef target) const noexcept;
        [[nodiscard]] SceneEditResult<std::vector<simulation::ecs::ComponentSchemaId>>
        components(SceneObjectRef target) const;
        [[nodiscard]] SceneEditResult<world::WorldObjectId> parent(SceneObjectRef target) const noexcept;
        [[nodiscard]] SceneEditResult<SceneComponentData> component(
            SceneObjectRef target,
            const simulation::ecs::ComponentSchemaId& schema
        ) const;
        [[nodiscard]] const SceneConfiguration& configuration() const noexcept;

    private:
        friend class SceneSession;
        SceneReadView(const SceneSource& source, sessions::ContentStamp stamp, sessions::EditGate& gate) noexcept
            : source_(&source), stamp_(stamp), gate_(gate)
        {}
        const SceneSource* source_;
        sessions::ContentStamp stamp_;
        sessions::EditGate& gate_;
    };
}
