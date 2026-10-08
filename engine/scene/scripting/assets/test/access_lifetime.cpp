#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <lux/engine/scene/scripting/ScriptAssetAccess.hpp>

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <semaphore>
#include <thread>

using namespace lux;
using namespace lux::scene::script;

namespace
{
    const asset::AssetId Id{*uuids::uuid::from_string("ffffffff-fedc-ba98-7654-321012345678")};
    constexpr ScriptAssetLimits Limits{1, 1, 24576, {8192, 16384, 8}};
    const std::thread::id OwnerThread = std::this_thread::get_id();
    std::unique_ptr<ScriptAssetAccess>* wake_owner{};

    struct Reply final : std::enable_shared_from_this<Reply>
    {
        using Result = script::ScriptAbilityErasedCompletion::CompletionResult;
        std::size_t calls{};
        bool blocked{};
        std::optional<ScriptAssetReadOutcome> value;
        std::function<void()> on_reply;
        std::function<void()> on_active;

        ScriptAssetScope::Completion completion() noexcept
        {
            return ScriptAssetScope::Completion::fromErased(script::ScriptAbilityErasedCompletion::bind(
                shared_from_this(),
                this,
                0,
                0,
                [](void* context, auto, auto, semantic::TypeId type, const void* data, std::uint32_t size
                ) noexcept -> Result
                {
                    auto& self = *static_cast<Reply*>(context);
                    ++self.calls;
                    assert(type == semantic::typeId(semantic::TTypeTraits<ScriptAssetReadOutcome>::CanonicalName));
                    assert(size == sizeof(ScriptAssetReadOutcome));
                    if (self.on_reply)
                    {
                        self.on_reply();
                    }
                    if (self.blocked)
                    {
                        return cxx::unexpected(script::EScriptAbilityCompletionError::BACKPRESSURE);
                    }
                    self.value = *static_cast<const ScriptAssetReadOutcome*>(data);
                    return {};
                },
                [](void*, auto, auto, script::ScriptAbilityOperationError) noexcept -> Result { std::terminate(); },
                [](void* context, auto, auto) noexcept
                {
                    auto& self = *static_cast<Reply*>(context);
                    if (self.on_active)
                    {
                        self.on_active();
                    }
                    return true;
                }
            ));
        }
    };

    cxx::SharedBytes<> image()
    {
        auto skeleton = std::make_shared<rdesc::Skeleton>();
        skeleton->bones = {{"root", -1, Eigen::Affine3f::Identity(), Eigen::Affine3f::Identity()}};
        auto value = asset::SkeletonAsset::create({Id, asset::SkeletonAsset::asset_type}, skeleton);
        assert(value);
        auto encoded = asset::TAssetSerDeser<asset::SkeletonAsset>::encode(**value, asset::AssetEncodeLimits{8192});
        assert(encoded);
        return cxx::SharedBytes<>::copyOf(*encoded);
    }

    void settle(process::ExecutionRuntime& runtime)
    {
        assert(runtime.waitUntil(
            [&]() noexcept
            {
                assert(runtime.dispatchTaskEvents());
                for (const auto& task : runtime.taskInfos())
                {
                    const bool pending =
                        task.state == process::ETaskState::QUEUED || task.state == process::ETaskState::RUNNING;
                    if (pending)
                    {
                        return false;
                    }
                }
                return !runtime.hasPendingWork();
            }
        ));
    }

    class BlockedProvider final : public asset::IAssetProvider
    {
    public:
        explicit BlockedProvider(cxx::SharedBytes<> image) : image_(std::move(image)) {}

        std::optional<asset::AssetId> resolve(std::string_view) const override
        {
            return Id;
        }

        bool contains(const asset::AssetId& id) const override
        {
            return id == Id;
        }

        cxx::expected<asset::AssetBlob, asset::EAssetStorageError> open(const asset::AssetId&, std::size_t)
            const override
        {
            entered.release();
            proceed.acquire();
            return asset::AssetBlob::fromShared(image_);
        }

        void enumerate(const std::function<void(const asset::ProviderEntry&)>&) const override {}

        std::optional<std::string> pathOf(const asset::AssetId&) const override
        {
            return "skeleton";
        }

        mutable std::binary_semaphore entered{0};
        mutable std::binary_semaphore proceed{0};

    private:
        cxx::SharedBytes<> image_;
    };

    void blockedOwner(bool typed, bool destroy_access, bool source_blocked)
    {
        auto runtime = process::ExecutionRuntime::create({1, 32, 32, {16}, process::BlockingSchedulerConfig{1, 16}});
        assert(runtime);
        process::TaskScope files{*runtime}, blocker{*runtime};
        asset::AssetVfs vfs;
        auto provider = std::make_shared<BlockedProvider>(image());
        auto mount = vfs.mount({"/Game", provider});
        assert(mount);
        auto endpoint = process::asset_loading::VfsAssetReadEndpoint::create(
            vfs.view().capture(),
            *runtime->blocking(),
            files,
            {2}
        );
        assert(endpoint);
        auto memory = process::asset_loading::makeAssetReadOverlay({{Id, {image()}}}, {});
        assert(memory);
        std::binary_semaphore entered{0}, proceed{0};
        if (!source_blocked)
        {
            assert(blocker.submit(
                {"Hold actual CPU decode", "LR05"},
                [&](process::TaskReporter) noexcept
                {
                    auto work = stdexec::then(
                        stdexec::schedule(runtime->cpu()),
                        [&]() noexcept
                        {
                            entered.release();
                            proceed.acquire();
                        }
                    );
                    return stdexec::upon_error(
                        std::move(work),
                        [](process::EExecutionError) noexcept { std::terminate(); }
                    );
                }
            ));
            assert(entered.try_acquire_for(std::chrono::seconds(5)));
        }
        auto access = ScriptAssetAccess::create(*runtime, source_blocked ? (*endpoint)->port() : *memory, Limits);
        assert(access);
        auto scope = (*access)->prepare({1, 1});
        assert(scope);
        auto reply = std::make_shared<Reply>();
        auto code = std::make_shared<int>(17);
        const std::weak_ptr<int> code_lifetime = code;
        if (typed)
        {
            assert((*scope)->readTyped<asset::SkeletonAsset>(Id, reply->completion(), code));
        }
        else
        {
            assert((*scope)->readAsset(Id, reply->completion()));
        }
        code.reset();
        if (source_blocked)
        {
            assert(provider->entered.try_acquire_for(std::chrono::seconds(5)));
        }
        std::mutex mutex;
        std::condition_variable condition;
        bool returned{};
        bool blocked{};
        std::thread watchdog{[&]()
                             {
                                 std::unique_lock lock{mutex};
                                 blocked =
                                     !condition.wait_for(lock, std::chrono::seconds(1), [&]() { return returned; });
                                 if (source_blocked)
                                 {
                                     provider->proceed.release();
                                 }
                                 else
                                 {
                                     proceed.release();
                                 }
                             }};
        if (destroy_access)
        {
            access->reset();
            assert((*scope)->retainedResults() == 0 && (*scope)->reservedBytes() == 0);
            assert((*scope)->readAsset(Id, reply->completion()).error() == EScriptAssetError::STOPPING);
        }
        else
        {
            const std::weak_ptr<ScriptAssetScope> weak = *scope;
            scope->reset();
            assert(weak.expired());
        }
        assert(!typed || !code_lifetime.expired());
        {
            std::lock_guard lock{mutex};
            returned = true;
        }
        condition.notify_one();
        watchdog.join();
        assert(!blocked);
        settle(*runtime);
        assert(reply->calls == 0 && code_lifetime.expired());
        scope->reset();
        if (*access)
        {
            auto replacement = (*access)->prepare({1, 1});
            assert(replacement && (*replacement)->reservedBytes() == 0);
        }
        access->reset();
        endpoint->reset();
        runtime->requestStop();
        assert(runtime->join());
    }

    void callbacks()
    {
        auto runtime = process::ExecutionRuntime::create({1, 32, 32, {16}});
        assert(runtime);
        auto memory = process::asset_loading::makeAssetReadOverlay({{Id, {image()}}}, {});
        assert(memory);
        for (bool deferred : {false, true})
        {
            auto access = ScriptAssetAccess::create(*runtime, *memory, Limits);
            assert(access);
            auto scope = (*access)->prepare({1, 1});
            assert(scope);
            auto reply = std::make_shared<Reply>();
            reply->blocked = deferred;
            auto remove = [&]() noexcept
            {
                (*access)->deliverCompletions();
                access->reset();
                assert((*scope)->reservedBytes() == 0 && (*scope)->retainedResults() == 0);
                scope->reset();
            };
            if (!deferred)
            {
                reply->on_reply = remove;
            }
            assert((*scope)->readTyped<asset::SkeletonAsset>(Id, reply->completion()));
            settle(*runtime);
            if (deferred)
            {
                assert(reply->calls == 1 && (*scope)->retainedResults() == 1);
                reply->blocked = false;
                reply->on_reply = remove;
                (*access)->deliverCompletions();
            }
            assert(!*access && !*scope && reply->calls == (deferred ? 2 : 1));
        }
        for (bool typed : {false, true})
        {
            auto access = ScriptAssetAccess::create(*runtime, *memory, Limits);
            assert(access);
            auto scope = (*access)->prepare({2, 1});
            assert(scope);
            auto reply = std::make_shared<Reply>();
            reply->on_active = [&]() noexcept
            {
                scope->reset();
                access->reset();
            };
            const auto rejected = typed ? (*scope)->readTyped<asset::SkeletonAsset>(Id, reply->completion())
                                        : (*scope)->readAsset(Id, reply->completion());
            assert(!rejected && rejected.error() == EScriptAssetError::STOPPING);
            assert(!*scope && !*access && reply->calls == 0);
        }
        for (bool typed : {false, true})
        {
            auto access = ScriptAssetAccess::create(*runtime, *memory, Limits);
            assert(access);
            auto scope = (*access)->prepare({3, 1});
            assert(scope);
            auto reply = std::make_shared<Reply>();
            wake_owner = &*access;
            runtime->setWake(+[]() noexcept
                             {
                                 if (std::this_thread::get_id() == OwnerThread)
                                 {
                                     wake_owner->reset();
                                 }
                             });
            const auto accepted = typed ? (*scope)->readTyped<asset::SkeletonAsset>(Id, reply->completion())
                                        : (*scope)->readAsset(Id, reply->completion());
            assert(accepted && !*access);
            runtime->setWake(nullptr);
            wake_owner = nullptr;
            assert((*scope)->retainedResults() == 0 && (*scope)->reservedBytes() == 0);
            settle(*runtime);
            assert(reply->calls == 0);
        }

        // Releasing a decoded result may destroy both public owners during explicit revoke.
        {
            auto access = ScriptAssetAccess::create(*runtime, *memory, Limits);
            assert(access);
            auto scope = (*access)->prepare({4, 1});
            assert(scope);
            auto reply = std::make_shared<Reply>();
            std::size_t cleanups{};
            auto code = std::shared_ptr<const void>{
                new int(1),
                [&](const void* value) noexcept
                {
                    delete static_cast<const int*>(value);
                    ++cleanups;
                    assert((*scope)->readAsset(Id, reply->completion()).error() == EScriptAssetError::STOPPING);
                    scope->reset();
                    access->reset();
                }
            };
            assert((*scope)->readTyped<asset::SkeletonAsset>(Id, reply->completion(), code));
            code.reset();
            settle(*runtime);
            assert(reply->calls == 1 && reply->value->succeeded() && cleanups == 0);
            (*scope)->revoke();
            assert(cleanups == 1 && !*scope && !*access);
        }

        // An external revoked Scope cannot keep the Access read provider alive.
        {
            std::shared_ptr<ScriptAssetScope> scope;
            auto reply = std::make_shared<Reply>();
            std::size_t cleanups{};
            auto bytes = std::shared_ptr<const std::vector<std::byte>>{
                new std::vector<std::byte>(8),
                [&](const std::vector<std::byte>* value) noexcept
                {
                    delete value;
                    ++cleanups;
                    assert(scope->readAsset(Id, reply->completion()).error() == EScriptAssetError::STOPPING);
                    assert(scope->reservedBytes() == 0 && scope->retainedResults() == 0);
                }
            };
            auto port = process::asset_loading::makeAssetReadOverlay(
                {{Id, {cxx::SharedBytes<>::fromOwner(bytes, std::span<const std::byte>{*bytes})}}},
                {}
            );
            assert(port);
            auto access = ScriptAssetAccess::create(*runtime, *port, Limits);
            assert(access);
            auto prepared = (*access)->prepare({5, 1});
            assert(prepared);
            scope = std::move(*prepared);
            bytes.reset();
            *port = {};
            access->reset();
            assert(cleanups == 1 && reply->calls == 0);
        }
        runtime->requestStop();
        assert(runtime->join());
    }
} // namespace

int main()
{
    for (bool typed : {false, true})
    {
        for (bool destroy_access : {false, true})
        {
            for (bool source_blocked : {false, true})
            {
                blockedOwner(typed, destroy_access, source_blocked);
            }
        }
    }
    callbacks();
    std::puts("PASS actual script assets: blocked IO/decode, Access/Scope death, code lifetime and callback reentry");
}
