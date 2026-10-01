#pragma once
#include <lux/engine/editor/sessions/SessionInstallation.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <stop_token>
namespace lux::editor::sessions
{
    struct SessionKindDescriptor final
    {
        SessionKindId kind;
        std::string label;
        std::vector<std::string> extensions;
    };
    struct SessionLoadInput final
    {
        asset::AssetVfsView source; // A captured/versioned source view, not mutable ProjectStorage.
        asset::AssetId asset;
        SourceBinding binding;
        std::optional<persistence::WriteTarget> target;
        std::size_t max_bytes{64 * 1024 * 1024};
    };
    class PreparedSessionData final
    {
    public:
        using Prepare = cxx::move_only_function<
            SessionFactoryResult<PreparedSessionInstallation>(SessionStore&, persistence::SaveService&)>;
        PreparedSessionData(contracts::CodeLease, Prepare);
        ~PreparedSessionData();
        PreparedSessionData(PreparedSessionData&&) noexcept;
        PreparedSessionData& operator=(PreparedSessionData&&) noexcept;
        PreparedSessionData(const PreparedSessionData&) = delete;
        PreparedSessionData& operator=(const PreparedSessionData&) = delete;
        // One use after admission; BUSY/WRONG_THREAD/capacity preflight keeps this result intact for retry.
        [[nodiscard]] SessionFactoryResult<PreparedSessionInstallation>
        prepare(SessionStore&, persistence::SaveService&) &&;
        [[nodiscard]] bool usesCode(const contracts::CodeLease&) const noexcept;

    private:
        struct Data;
        std::unique_ptr<Data> data_;
    };
    class SessionFactoryEntry final
    {
    public:
        using Decode = cxx::move_only_function<SessionFactoryResult<
            PreparedSessionData>(const SessionLoadInput&, std::span<const std::byte>, std::stop_token)>;
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
        [[nodiscard]] std::span<const std::shared_ptr<SessionFactoryEntry>> entries() const noexcept;

    private:
        struct Data;
        std::shared_ptr<const Data> data_;
    };
    // Submit run() through the existing Process blocking scheduler. It owns only input/entry/code.
    // Completion carries PreparedSessionData back to the owner; no Session or UI is made on the worker.
    class SessionLoadJob final
    {
    public:
        SessionLoadJob(std::shared_ptr<SessionFactoryEntry>, SessionLoadInput);
        SessionLoadJob(const SessionLoadJob&) = delete;
        SessionLoadJob& operator=(const SessionLoadJob&) = delete;
        SessionLoadJob(SessionLoadJob&&) noexcept = default;
        SessionLoadJob& operator=(SessionLoadJob&&) = delete;
        [[nodiscard]] SessionFactoryResult<PreparedSessionData> run(std::stop_token = {}) &&;

    private:
        std::shared_ptr<SessionFactoryEntry> entry_;
        SessionLoadInput input_;
    };
}
