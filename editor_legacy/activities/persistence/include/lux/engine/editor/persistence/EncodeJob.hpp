#pragma once

#include <lux/engine/object/CodeLease.hpp>
#include <lux/engine/editor/persistence/SaveTypes.hpp>

namespace lux::editor::persistence
{
    class IEncodeJob
    {
    public:
        virtual ~IEncodeJob() = default;
        [[nodiscard]] virtual PersistenceResult<EncodedArtifact> encode(std::stop_token stop) = 0;
    };
    // The outer owner remains alive until the plugin job's virtual destructor has returned.
    struct OwnedEncodeJob final
    {
        lux::object::CodeLease code{lux::object::CodeLease::builtin()};
        std::unique_ptr<IEncodeJob> job;
        OwnedEncodeJob() = default;
        OwnedEncodeJob(lux::object::CodeLease lease, std::unique_ptr<IEncodeJob> value) noexcept
            : code(std::move(lease)), job(std::move(value))
        {}
        OwnedEncodeJob(OwnedEncodeJob&&) noexcept = default;
        OwnedEncodeJob& operator=(OwnedEncodeJob&& other) noexcept
        {
            OwnedEncodeJob previous(std::move(other));
            using std::swap;
            swap(code, previous.code);
            swap(job, previous.job);
            return *this;
        }
        [[nodiscard]] PersistenceResult<EncodedArtifact> encode(std::stop_token stop) noexcept;
    };
    class IPreparedRebind
    {
    public:
        virtual ~IPreparedRebind() = default;
        [[nodiscard]] virtual EAdoption apply(SaveReceipt&&) noexcept = 0;
    };
    struct FrozenSave final
    {
        SaveSourceInfo source;
        std::size_t retained_bytes{};
        OwnedEncodeJob encoding;
        // Owner-only permit/candidate. Never enters the encoding work item.
        std::unique_ptr<IPreparedRebind> rebind;
        FrozenSave() = default;
        FrozenSave(
            SaveSourceInfo info,
            std::size_t bytes,
            OwnedEncodeJob job,
            std::unique_ptr<IPreparedRebind> candidate
        )
            : source(std::move(info)), retained_bytes(bytes), encoding(std::move(job)), rebind(std::move(candidate))
        {}
        FrozenSave(FrozenSave&&) noexcept = default;
        FrozenSave& operator=(FrozenSave&& other) noexcept
        {
            FrozenSave previous(std::move(other));
            using std::swap;
            swap(source, previous.source);
            swap(retained_bytes, previous.retained_bytes);
            swap(encoding, previous.encoding);
            swap(rebind, previous.rebind);
            return *this;
        }
        ~FrozenSave()
        {
            // Rejected/cancelled owner-side jobs release plugin inputs before releasing a rebind permit.
            encoding = {};
            rebind.reset();
        }
    };
    struct EncodeWork final
    {
        SaveId id;
        OwnedEncodeJob encoding;
    };
}
