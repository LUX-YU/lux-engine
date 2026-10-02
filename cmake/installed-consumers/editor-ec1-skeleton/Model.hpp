#pragma once
#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/editing/EditExecutor.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <lux/cxx/core/function_ref.hpp>

namespace skeleton
{
    using namespace lux;
    using namespace lux::editor;
    inline const sessions::SessionKindId kind{"example.skeleton"};
    inline constexpr std::size_t max_bytes = 16 * 1024 * 1024;

    // The plugin owns only this domain's data and operations. Store, gate, log and save execution are SDK owners.
    class Session final : public sessions::IEditSession
    {
    public:
        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;
        Session(Session &&) = delete;
        Session &operator=(Session &&) = delete;
        static sessions::SessionResult<std::unique_ptr<Session>> create(sessions::SessionId,
                                                                        sessions::SourceBinding,
                                                                        std::shared_ptr<const asset::SkeletonAsset>);
        sessions::SessionInfo describe() const override;
        sessions::SessionResult<void> edit(sessions::ContentStamp, std::size_t bone, std::string name, float x);
        sessions::SessionResult<void> replay(bool forward);
        sessions::SessionResult<rdesc::Skeleton> read() const;
        sessions::SessionResult<void> withRead(cxx::function_ref<void()> callback) const;
        sessions::BindingRevision bindingRevision() const noexcept { return state_.bindingRevision(); }
        editing::EditResult<editing::HistoryView> history() const noexcept { return history_->view(); }
        sessions::SessionResult<std::shared_ptr<const asset::SkeletonAsset>> freeze(asset::AssetId) const;
        sessions::SessionResult<void> adopt(sessions::ContentStamp, Session &candidate);

    private:
        friend class SaveSource;
        friend class Rebind;
        friend class Operation;
        friend class Edit;
        Session(sessions::SessionId,
                sessions::SourceBinding,
                std::shared_ptr<const asset::SkeletonAsset>,
                std::unique_ptr<editing::EditHistory>);
        sessions::ContentStamp currentContent() const noexcept override;
        sessions::SessionResult<sessions::ClosePermit> prepareClose(sessions::ContentStamp) noexcept override;
        mutable sessions::SessionState state_;
        asset::AssetInfo info_;
        std::vector<asset::AssetAuxiliaryPayload> auxiliary_;
        rdesc::Skeleton source_;
        std::unique_ptr<editing::EditHistory> history_;
    };
    std::shared_ptr<sessions::SessionFactoryEntry> factory(contracts::CodeLease);
} // namespace skeleton
