#include <lux/engine/scene/scripting/ScriptAssetAccess.hpp>
#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>

#include <cassert>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <semaphore>
#include <thread>

using namespace lux;
using namespace lux::scene::script;
void qualifyInstanceBindings();

namespace
{
    class CountedRead final : public process::asset_loading::AssetReadPort::Endpoint
    {
    public:
        explicit CountedRead(process::asset_loading::AssetReadPort port) : port_(std::move(port)) {}

        lux::async::SubmitResult submit(process::asset_loading::ReadAssetImage request, void* state,
            void (*complete)(void*, Outcome&&) noexcept, lux::async::SubmitOptions options) noexcept override
        {
            ++calls;
            if (fail_io)
            {
                complete(state, cxx::unexpected(lux::async::TOperationFailure<asset::EAssetStorageError>::domain(
                    asset::EAssetStorageError::IO_FAILURE)));
                return {};
            }
            return port_.submit(request, state, complete, options);
        }

        std::atomic_size_t calls{};
        bool fail_io{};
    private:
        process::asset_loading::AssetReadPort port_;
    };
    struct Reply final : std::enable_shared_from_this<Reply>
    {
        std::optional<ScriptAssetReadOutcome> value;
        bool active{true};
        bool backpressure{};
        std::size_t calls{};
        ScriptAssetScope::Completion completion()
        {
            return ScriptAssetScope::Completion::fromErased(script::ScriptAbilityErasedCompletion::bind(
                shared_from_this(), this, 0, 0,
                [](void* context, auto, auto, semantic::TypeId type, const void* data, std::uint32_t size) noexcept
                    -> script::ScriptAbilityErasedCompletion::CompletionResult {
                    auto& reply = *static_cast<Reply*>(context);
                    ++reply.calls;
                    if (!reply.active)
                        return cxx::unexpected(script::EScriptAbilityCompletionError::STALE);
                    if (reply.backpressure)
                        return cxx::unexpected(script::EScriptAbilityCompletionError::BACKPRESSURE);
                    assert(type == semantic::typeId(semantic::TTypeTraits<ScriptAssetReadOutcome>::CanonicalName));
                    assert(size == sizeof(ScriptAssetReadOutcome));
                    reply.value = *static_cast<const ScriptAssetReadOutcome*>(data);
                    return {};
                },
                [](void*, auto, auto, script::ScriptAbilityOperationError) noexcept
                    -> script::ScriptAbilityErasedCompletion::CompletionResult { std::terminate(); },
                [](void* context, auto, auto) noexcept { return static_cast<Reply*>(context)->active; }
            ));
        }
    };
    void settle(process::ExecutionRuntime& execution)
    {
        assert(execution.waitUntil([&]() noexcept {
            assert(execution.dispatchTaskEvents());
            for (const auto& task : execution.taskInfos())
                if (task.state == process::ETaskState::QUEUED || task.state == process::ETaskState::RUNNING)
                    return false;
            return !execution.hasPendingWork();
        }));
    }
}

int main()
{
    qualifyInstanceBindings();
    static_assert(std::is_trivially_copyable_v<ScriptAssetHandle>);
    static_assert(std::is_trivially_copyable_v<ScriptAssetReadOutcome>);
    static_assert(!std::is_copy_constructible_v<ScriptAssetScope>);
    static_assert(!std::is_move_constructible_v<ScriptAssetScope>);
    static_assert(!std::is_copy_constructible_v<simulation::script::ScriptApiInstanceBinding>);
    static_assert(!std::is_move_assignable_v<simulation::script::ScriptApiInstanceBinding>);
    const asset::AssetId id{*uuids::uuid::from_string("ffffffff-fedc-ba98-7654-321012345678")};
    const asset::AssetId missing{*uuids::uuid::from_string("ffffffff-fedc-ba98-7654-321012345679")};
    auto skeleton = std::make_shared<rdesc::Skeleton>();
    skeleton->bones = {{"root", -1, Eigen::Affine3f::Identity(), Eigen::Affine3f::Identity()}};
    auto asset = asset::SkeletonAsset::create({id, asset::SkeletonAsset::asset_type}, skeleton);
    assert(asset);
    auto encoded = asset::TAssetSerDeser<asset::SkeletonAsset>::encode(**asset, asset::AssetEncodeLimits{8192});
    assert(encoded);
    auto encoded_owner = std::make_shared<const std::vector<std::byte>>(std::move(*encoded));
    auto image = cxx::SharedBytes<>::fromOwner(encoded_owner, std::span<const std::byte>{*encoded_owner});
    auto execution = process::ExecutionRuntime::create({1, 64, 64, {32}, process::BlockingSchedulerConfig{1, 64}});
    assert(execution);
    process::TaskScope file_tasks{*execution};
    asset::AssetVfs vfs;
    auto blocking = execution->blocking();
    assert(blocking);
    auto endpoint = process::asset_loading::VfsAssetReadEndpoint::create(
        vfs.view().capture(), *blocking, file_tasks, {4}
    );
    assert(endpoint);
    auto reads = process::asset_loading::makeAssetReadOverlay({{id, {image}}}, (*endpoint)->port());
    assert(reads);

    for (const std::size_t capacity : {1U, 2U})
    {
        ScriptAssetLimits limits{2, capacity, capacity * 24576, {8192, 16384, 8}};
        auto access = ScriptAssetAccess::create(*execution, *reads, limits);
        assert(access);
        auto first = (*access)->prepare({1, 1});
        auto second = (*access)->prepare({2, 1});
        assert(first && second);
        assert((*access)->prepare({3, 1}).error() == EScriptAssetError::CAPACITY_EXCEEDED);
        std::vector<std::shared_ptr<Reply>> replies;
        for (std::size_t i{}; i < capacity; ++i)
        {
            auto reply = std::make_shared<Reply>();
            assert((*first)->readAsset(id, reply->completion()));
            replies.push_back(std::move(reply));
        }
        auto rejected = std::make_shared<Reply>();
        assert((*first)->readAsset(id, rejected->completion()).error() == EScriptAssetError::CAPACITY_EXCEEDED);
        settle(*execution);
        assert(rejected->calls == 0);
        for (const auto& reply : replies)
        {
            assert(reply->calls == 1 && reply->value->succeeded());
            const auto handle = reply->value->handle();
            const auto description = (*first)->describeAsset(handle);
            assert(description && description->id == id && description->image_bytes == image.size());
            assert(description->has_image && !description->decoded);
            const auto typed_read = (*first)->withAsset<asset::SkeletonAsset>(handle, [](const auto&) noexcept {
                std::abort();
            });
            assert(!typed_read && typed_read.error() == EScriptAssetError::TYPE_MISMATCH);
            assert((*second)->describeAsset(handle).error() == EScriptAssetError::INVALID_HANDLE);
            auto bytes = (*first)->copyAssetBytes(handle, 0, 8);
            const bool matches = bytes && bytes->size == 8 &&
                std::equal(bytes->bytes.begin(), bytes->bytes.begin() + 8, image.data());
            assert(matches);
            assert((*first)->copyAssetBytes(handle, image.size(), 0)->size == 0);
            assert((*first)->copyAssetBytes(handle, UINT64_MAX, 2).error() == EScriptAssetError::INVALID_RANGE);
            assert((*first)->copyAssetBytes(handle, 0, 257).error() == EScriptAssetError::INVALID_RANGE);
            assert((*first)->releaseAsset(handle));
            assert((*first)->releaseAsset(handle).error() == EScriptAssetError::INVALID_HANDLE);
        }
        assert((*first)->reservedBytes() == 0 && (*first)->retainedResults() == 0);
        auto typed = std::make_shared<Reply>();
        auto code = std::make_shared<int>(73);
        std::weak_ptr<int> weak_code = code;
        assert((*first)->readTyped<asset::SkeletonAsset>(id, typed->completion(), std::move(code)));
        settle(*execution);
        assert(typed->value && typed->value->succeeded() && !weak_code.expired());
        const auto typed_handle = typed->value->handle();
        assert(typed_handle != replies.back()->value->handle());
        const auto typed_description = (*first)->describeAsset(typed_handle);
        assert(typed_description && typed_description->decoded && !typed_description->has_image);
        assert(typed_description->type == asset::SkeletonAsset::asset_type && typed_description->image_bytes == 0);
        assert((*first)->copyAssetBytes(typed_handle, 0, 0).error() == EScriptAssetError::TYPE_MISMATCH);
        bool inspected{};
        assert((*first)->withAsset<asset::SkeletonAsset>(typed_handle, [&](const auto& value) noexcept {
            inspected = value.id() == id && value.data().bones.size() == 1;
            // The local borrow owns code and asset through a reentrant release.
            assert((*first)->releaseAsset(typed_handle));
            assert(!weak_code.expired() && value.data().bones[0].name == "root");
        }));
        assert(inspected && weak_code.expired());
        auto absent = std::make_shared<Reply>();
        assert((*first)->readAsset(missing, absent->completion()));
        settle(*execution);
        assert(absent->value && !absent->value->succeeded() && !absent->value->handle().valid());
        assert(absent->value->errorDomain() == EScriptAssetFailureDomain::STORAGE);
        assert((*first)->lastFailure()->storage_error == asset::EAssetStorageError::NOT_FOUND);

        // Ingress capacity belongs to ScriptSystem. Its temporary refusal cannot discard native completion.
        auto blocked = std::make_shared<Reply>();
        blocked->backpressure = true;
        assert((*first)->readAsset(id, blocked->completion()));
        settle(*execution);
        assert(blocked->calls == 1 && !blocked->value && (*first)->retainedResults() == 1);
        const auto reserved_bytes = (*first)->reservedBytes();
        (*access)->deliverCompletions();
        assert(blocked->calls == 2 && (*first)->reservedBytes() == reserved_bytes);
        blocked->backpressure = false;
        (*access)->deliverCompletions();
        assert(blocked->value && blocked->value->succeeded() && blocked->calls == 3);
        (*access)->deliverCompletions();
        assert(blocked->calls == 3 && (*first)->releaseAsset(blocked->value->handle()));

        auto blocked_failure = std::make_shared<Reply>();
        blocked_failure->backpressure = true;
        assert((*first)->readAsset(missing, blocked_failure->completion()));
        settle(*execution);
        assert(!blocked_failure->value && (*first)->retainedResults() == 1);
        blocked_failure->backpressure = false;
        (*access)->deliverCompletions();
        assert(blocked_failure->value && !blocked_failure->value->succeeded());
        assert((*first)->retainedResults() == 0 && (*first)->reservedBytes() == 0);

        // Completion stale after admission: no result slot survives a rejected delivery.
        auto stale = std::make_shared<Reply>();
        assert((*first)->readAsset(id, stale->completion()));
        stale->active = false;
        settle(*execution);
        assert(stale->calls == 1 && !stale->value && (*first)->reservedBytes() == 0);

        // Completion accepted, but no script resume yet. Revoke invalidates that retained result.
        auto delivered = std::make_shared<Reply>();
        assert((*first)->readAsset(id, delivered->completion()));
        settle(*execution);
        assert(delivered->value && delivered->value->succeeded());
        (*first)->revoke();
        assert((*first)->describeAsset(delivered->value->handle()).error() == EScriptAssetError::INVALID_HANDLE);
        assert((*first)->readAsset(id, delivered->completion()).error() == EScriptAssetError::STOPPING);

        // Owner retirement before completion dispatch leaves accepted work in the original runtime.
        auto late = std::make_shared<Reply>();
        assert((*second)->readTyped<asset::SkeletonAsset>(id, late->completion()));
        (*second)->revoke();
        settle(*execution);
        assert(!late->value && late->calls == 0 && (*second)->retainedResults() == 0);
        first->reset();
        second->reset();
        auto replacement = (*access)->prepare({1, 2});
        assert(replacement);
        assert((*replacement)->describeAsset(delivered->value->handle()).error() == EScriptAssetError::INVALID_HANDLE);
        access->reset();
        assert((*replacement)->readAsset(id, late->completion()).error() == EScriptAssetError::STOPPING);
    }

    {
        auto counted = std::make_shared<CountedRead>(*reads);
        auto access = ScriptAssetAccess::create(*execution, process::asset_loading::AssetReadPort{counted},
            {1, 1, 24576, {8192, 16384, 8}});
        assert(access);
        auto scope = (*access)->prepare({9, 1}); assert(scope);
        auto reply = std::make_shared<Reply>();
        assert((*scope)->readAsset(id, reply->completion())); settle(*execution);
        assert(reply->value && reply->value->succeeded() && counted->calls == 1);
        const auto handle = reply->value->handle();
        for (std::size_t i = 0; i != 10000; ++i)
        {
            assert((*scope)->describeAsset(handle)->image_bytes == image.size());
            assert((*scope)->copyAssetBytes(handle, 0, 8)->size == 8);
        }
        assert(counted->calls == 1 && (*scope)->retainedResults() == 1);
        assert((*scope)->releaseAsset(handle));
        auto wrong = std::make_shared<Reply>();
        assert((*scope)->readTyped<asset::MeshAsset>(id, wrong->completion())); settle(*execution);
        assert(wrong->value && wrong->value->errorDomain() == EScriptAssetFailureDomain::DECODE);
        assert((*scope)->lastFailure()->code == process::asset_loading::EAssetLoadError::DECODE_FAILURE);
        assert((*scope)->retainedResults() == 0 && counted->calls == 2);
        counted->fail_io = true;
        auto io = std::make_shared<Reply>();
        assert((*scope)->readAsset(id, io->completion())); settle(*execution);
        assert(io->value && io->value->errorDomain() == EScriptAssetFailureDomain::STORAGE);
        assert(io->value->errorCode() == static_cast<std::uint32_t>(asset::EAssetStorageError::IO_FAILURE));
        assert((*scope)->retainedResults() == 0 && counted->calls == 3);
        std::cout << "EC2 counts: warm queries=20000 extra_reads=0 retained=1; "
                     "wrong-type reads=1; IO-failure reads=1; final retained=0\n";
    }
    // A malformed image and a transport limit are distinct accepted outcomes, not admission failures.
    {
        auto truncated = process::asset_loading::makeAssetReadOverlay({{id, {image.subspan(0, 1)}}}, *reads);
        assert(truncated);
        auto access = ScriptAssetAccess::create(*execution, *truncated, {1, 1, 24576, {8192, 16384, 8}});
        assert(access);
        auto scope = (*access)->prepare({10, 1}); assert(scope);
        auto broken = std::make_shared<Reply>();
        assert((*scope)->readTyped<asset::SkeletonAsset>(id, broken->completion())); settle(*execution);
        assert(broken->value && broken->value->errorDomain() == EScriptAssetFailureDomain::DECODE);
        auto small = ScriptAssetAccess::create(*execution, *reads, {1, 1, 8193, {1, 8192, 8}});
        assert(small);
        auto small_scope = (*small)->prepare({10, 2}); assert(small_scope);
        auto oversized = std::make_shared<Reply>();
        assert((*small_scope)->readAsset(id, oversized->completion())); settle(*execution);
        assert(oversized->value && oversized->value->errorDomain() == EScriptAssetFailureDomain::STORAGE);
        assert(oversized->value->errorCode() == static_cast<std::uint32_t>(asset::EAssetStorageError::LIMIT_EXCEEDED));
        assert((*scope)->retainedResults() == 0 && (*small_scope)->reservedBytes() == 0);
    }
    // Hold the actual blocking scheduler: revoke while the accepted VFS read has not executed.
    {
        process::TaskScope blocker{*execution};
        std::binary_semaphore entered{0}, proceed{0};
        assert(blocker.submit({"Hold IO", "test"}, [&](process::TaskReporter) noexcept {
            auto work = stdexec::then(stdexec::schedule(*blocking), [&]() noexcept {
                entered.release();
                proceed.acquire();
            });
            return stdexec::upon_error(std::move(work), [](process::EExecutionError) noexcept { std::terminate(); });
        }));
        assert(entered.try_acquire_for(std::chrono::seconds(5)));
        auto access = ScriptAssetAccess::create(*execution, (*endpoint)->port(), {1, 1, 24576, {8192, 16384, 8}});
        assert(access);
        auto scope = (*access)->prepare({4, 1});
        assert(scope);
        auto reply = std::make_shared<Reply>();
        assert((*scope)->readAsset(missing, reply->completion()));
        (*scope)->revoke();
        proceed.release();
        settle(*execution);
        assert(reply->calls == 0 && (*scope)->retainedResults() == 0 && (*scope)->reservedBytes() == 0);
    }
    // The memory endpoint supplies bytes synchronously; CPU decode is still blocked and must not run after revoke.
    {
        process::TaskScope blocker{*execution};
        std::binary_semaphore entered{0}, proceed{0};
        assert(blocker.submit({"Hold decode", "test"}, [&](process::TaskReporter) noexcept {
            auto work = stdexec::then(stdexec::schedule(execution->cpu()), [&]() noexcept {
                entered.release();
                proceed.acquire();
            });
            return stdexec::upon_error(std::move(work), [](process::EExecutionError) noexcept { std::terminate(); });
        }));
        assert(entered.try_acquire_for(std::chrono::seconds(5)));
        auto access = ScriptAssetAccess::create(*execution, *reads, {1, 1, 24576, {8192, 16384, 8}});
        assert(access);
        auto scope = (*access)->prepare({4, 2});
        assert(scope);
        auto reply = std::make_shared<Reply>();
        assert((*scope)->readTyped<asset::SkeletonAsset>(id, reply->completion()));
        (*scope)->revoke();
        proceed.release();
        settle(*execution);
        assert(reply->calls == 0 && (*scope)->retainedResults() == 0 && (*scope)->reservedBytes() == 0);
    }
    execution->requestStop();
    assert(execution->join());
    std::cout << "PASS native script asset scopes: capacity 1/2, codec, code lifetime, "
                 "aliases, stale and stopped completion\n";
}
