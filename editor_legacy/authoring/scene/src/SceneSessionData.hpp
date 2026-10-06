#pragma once
#include <lux/engine/editor/editing/EditOperation.hpp>

#include "SceneSourceData.hpp"
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <thread>

namespace lux::editor::scene
{
    namespace detail
    {
        struct SceneChangeRecord final
        {
            sessions::ObservationVersion observed;
            std::vector<world::WorldObjectId> objects;
            bool structure{};
            bool configuration{};
            [[nodiscard]] std::size_t bytes() const noexcept
            {
                return sizeof(*this) + objects.capacity() * sizeof(world::WorldObjectId);
            }
        };
    }

    struct SceneSession::Impl final
    {
        const std::thread::id owner{std::this_thread::get_id()};
        SceneSessionLimits limits;
        sessions::SessionState state;
        SceneSource source;
        std::unique_ptr<editing::EditHistory> history;
        std::vector<detail::SceneChangeRecord> changes;
        std::size_t change_bytes{};
        sessions::ObservationVersion oldest;

        Impl(sessions::SessionId id, sessions::SourceBinding binding, SceneSource value, SceneSessionLimits policy)
            : limits(policy), state(id, std::move(binding)), source(std::move(value))
        {
            changes.reserve(limits.change_records);
        }
        [[nodiscard]] SceneEditResult<void> available() const noexcept;
        [[nodiscard]] sessions::ContentStamp content() const noexcept;
        [[nodiscard]] SceneChangeCursor cursor() const noexcept;
        void publish(detail::SceneChangeRecord change) noexcept;
        [[nodiscard]] SceneEditResult<SceneEditReceipt> replay(bool forward);
    };

    namespace detail
    {
        struct SceneSessionAccess final
        {
            using Data = SceneSession::Impl;
            static Data& data(SceneSession& session) noexcept
            {
                return *session.impl_;
            }
        };
        struct SceneObjectDelta final
        {
            world::WorldObjectId id;
            std::optional<SceneObjectData> before;
            std::optional<SceneObjectData> after;
        };
        struct SceneFieldDelta final
        {
            world::WorldObjectId object;
            SceneComponentData before;
            SceneComponentData after;
            std::shared_ptr<const void> code;
            SwapSceneComponent swap{};
        };
        struct ScenePreparedInput final
        {
            std::vector<SceneObjectDelta> objects;
            std::vector<SceneFieldDelta> fields;
            std::optional<SceneConfiguration> configuration;
            std::vector<SceneObjectData> initial_objects;
            SceneChangeRecord change;
            std::size_t retained_bytes{};
        };
        [[nodiscard]] SceneEditResult<editing::EditOperationPtr> makeSceneEdit(
            SceneSessionAccess::Data& owner,
            SceneEditBatch batch
        );
        [[nodiscard]] inline SceneEditError historyFailure(const editing::EditFailure& failure) noexcept
        {
            SceneEditError result{ESceneEditError::HISTORY};
            result.history = failure;
            return result;
        }
        [[nodiscard]] inline editing::EditFailure editFailure(const SceneEditError& failure) noexcept
        {
            return editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(failure.code),
                "Scene candidate rejected"
            );
        }
    }
}
