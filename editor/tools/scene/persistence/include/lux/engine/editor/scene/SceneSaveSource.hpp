#pragma once
#include <lux/engine/editor/scene/SceneSessionAccess.hpp>
#include <lux/engine/editor/scene/ScenePersistenceAccess.hpp>
#include <lux/engine/editor/persistence/SaveSource.hpp>
namespace lux::editor::scene
{
    class SceneSaveSource final : public persistence::ISaveSource
    {
    public:
        SceneSaveSource(
            SceneSessionAccess access,
            sessions::TSessionKey<SceneSession> key,
            std::optional<persistence::WriteTarget> target,
            sessions::BindingRevision binding
        ) noexcept
            : access_(access), key_(key), target_(std::move(target)), target_binding_(binding)
        {}
        [[nodiscard]] persistence::PersistenceResult<persistence::SaveSourceInfo> describe() const override;
        [[nodiscard]] persistence::PersistenceResult<persistence::FrozenSave> captureForSave(
            const persistence::SaveSourceInfo&,
            const persistence::SaveRequest&,
            std::size_t max_bytes
        ) override;
        [[nodiscard]] persistence::EAdoption accept(persistence::SaveReceipt&&) noexcept override;

    private:
        SceneSessionAccess access_;
        sessions::TSessionKey<SceneSession> key_;
        std::optional<persistence::WriteTarget> target_;
        sessions::BindingRevision target_binding_;
    };
}
