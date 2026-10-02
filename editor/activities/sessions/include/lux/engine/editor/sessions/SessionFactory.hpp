#pragma once
#include <lux/engine/editor/sessions/SessionInstallation.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/resource/asset/AssetTypeId.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <stop_token>
namespace lux::editor::sessions
{
    // Exact source-format relationship and save naming policy. A file suffix is only a discovery hint.
    struct SourceAuthoring final
    {
        std::string canonical_name;
        std::uint32_t version{1};
        std::string save_extension;
        bool is_default{true};
        [[nodiscard]] asset::AssetTypeId type() const noexcept
        {
            return asset::AssetTypeId::fromName(canonical_name);
        }
    };
    struct SessionKindDescriptor final
    {
        SessionKindId kind;
        std::string label;
        std::vector<std::string> extensions;
        std::optional<SourceAuthoring> source;
    };
    struct SessionLoadInput final
    {
        asset::AssetVfsView source; // A captured/versioned source view, not mutable ProjectStorage.
        asset::AssetId asset;
        SourceBinding binding;
        std::optional<persistence::WriteTarget> target;
        std::size_t max_bytes{64 * 1024 * 1024};
        std::optional<ContentStamp> reload;
    };
    class PreparedSessionReload final
    {
    public:
        // Only the already prepared, checked domain swap; no extension/user callbacks in this function.
        using Adopt = cxx::move_only_function<SessionFactoryResult<ContentStamp>(SessionStore&)>;
        PreparedSessionReload(contracts::CodeLease, SessionId, Adopt, std::unique_ptr<persistence::ISaveSource>);
        ~PreparedSessionReload();
        PreparedSessionReload(PreparedSessionReload&&) noexcept;
        PreparedSessionReload& operator=(PreparedSessionReload&&) noexcept;
        PreparedSessionReload(const PreparedSessionReload&) = delete;
        PreparedSessionReload& operator=(const PreparedSessionReload&) = delete;

    private:
        friend class InstalledSession;
        contracts::CodeLease code_;
        SessionId session_;
        Adopt adopt_;
        std::unique_ptr<persistence::ISaveSource> source_;
    };
    class SessionPreparation final
    {
    public:
        using Prepare = cxx::move_only_function<
            SessionFactoryResult<PreparedSessionInstallation>(SessionStore&, persistence::SaveService&)>;
        using Reload = cxx::move_only_function<SessionFactoryResult<PreparedSessionReload>(SessionStore&)>;
        SessionPreparation(contracts::CodeLease, Prepare);
        SessionPreparation(contracts::CodeLease, ContentStamp, Reload);
        ~SessionPreparation();
        SessionPreparation(SessionPreparation&&) noexcept;
        SessionPreparation& operator=(SessionPreparation&&) noexcept;
        SessionPreparation(const SessionPreparation&) = delete;
        SessionPreparation& operator=(const SessionPreparation&) = delete;
        // One use after admission; BUSY/WRONG_THREAD/capacity preflight keeps this result intact for retry.
        [[nodiscard]] SessionFactoryResult<PreparedSessionInstallation>
        prepare(SessionStore&, persistence::SaveService&) &&;
        [[nodiscard]] SessionFactoryResult<PreparedSessionReload> prepareReload(SessionStore&) &&;
        [[nodiscard]] bool usesCode(const contracts::CodeLease&) const noexcept;

    private:
        struct Data;
        std::unique_ptr<Data> data_;
    };
    class SessionFactoryEntry final
    {
    public:
        using Decode = cxx::move_only_function<SessionFactoryResult<
            SessionPreparation>(const SessionLoadInput&, std::span<const std::byte>, std::stop_token)>;
        SessionFactoryEntry(contracts::CodeLease, SessionKindDescriptor, Decode);
        ~SessionFactoryEntry();
        SessionFactoryEntry(const SessionFactoryEntry&) = delete;
        SessionFactoryEntry& operator=(const SessionFactoryEntry&) = delete;
        SessionFactoryEntry(SessionFactoryEntry&&) = delete;
        SessionFactoryEntry& operator=(SessionFactoryEntry&&) = delete;
        [[nodiscard]] const SessionKindDescriptor& descriptor() const noexcept;
        [[nodiscard]] bool usesCode(const contracts::CodeLease& code) const noexcept
        {
            return code_.sameOwner(code);
        }

    private:
        friend class SessionFactorySnapshot;
        friend class SessionLoadJob;
        contracts::CodeLease code_;
        SessionKindDescriptor descriptor_;
        Decode decode_;
    };
    class SessionFactorySnapshot final
    {
    public:
        [[nodiscard]] static SessionFactoryResult<SessionFactorySnapshot> create(
            std::vector<std::shared_ptr<SessionFactoryEntry>>,
            std::size_t capacity = 256
        );
        [[nodiscard]] SessionFactoryResult<std::shared_ptr<SessionFactoryEntry>> find(SessionKindId) const;
        // Immutable registration index; no factory callback runs during lookup. Multiple defaults or
        // multiple non-default candidates require an explicit choice, never registration-order selection.
        [[nodiscard]] SessionFactoryResult<std::shared_ptr<SessionFactoryEntry>> selectSource(
            std::string_view canonical_name,
            std::uint32_t version,
            std::optional<SessionKindId> preferred = {}
        ) const;
        [[nodiscard]] std::span<const std::shared_ptr<SessionFactoryEntry>> entries() const noexcept;

    private:
        struct Data;
        std::shared_ptr<const Data> data_;
    };
    // Submit run() through the existing Process blocking scheduler. It owns only input/entry/code.
    // Completion carries SessionPreparation back to the owner; no Session or UI is made on the worker.
    class SessionLoadJob final
    {
    public:
        SessionLoadJob(std::shared_ptr<SessionFactoryEntry>, SessionLoadInput);
        SessionLoadJob(const SessionLoadJob&) = delete;
        SessionLoadJob& operator=(const SessionLoadJob&) = delete;
        SessionLoadJob(SessionLoadJob&&) noexcept = default;
        SessionLoadJob& operator=(SessionLoadJob&&) = delete;
        [[nodiscard]] SessionFactoryResult<SessionPreparation> run(std::stop_token = {}) &&;

    private:
        std::shared_ptr<SessionFactoryEntry> entry_;
        SessionLoadInput input_;
    };
}
