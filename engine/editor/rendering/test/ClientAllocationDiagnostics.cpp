// Compiled only into the diagnostic render_client DLL, next to its allocation replacements.
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <cstddef>
#include <limits>

extern "C" void lux_er1_client_allocation_fail_after(std::size_t) noexcept;
extern "C" std::size_t lux_er1_client_allocation_disarm() noexcept;

extern "C" __declspec(dllexport) unsigned lux_er1_client_allocation_tests(std::size_t *failures) noexcept
{
    using namespace lux::render;
    *failures = 0;
    try
    {
        AlignedAllocator<std::byte, 64> allocator;
        for (const auto size : {std::size_t{0}, std::size_t{1}, std::size_t{65}})
        {
            auto *data = allocator.allocate(size);
            const bool aligned = reinterpret_cast<std::uintptr_t>(data) % 64 == 0;
            allocator.deallocate(data, size);
            if (!aligned)
                return 1;
        }
        bool overflow{};
        try
        {
            AlignedAllocator<std::uint64_t, 64> wide;
            auto *data = wide.allocate((std::numeric_limits<std::size_t>::max)());
            wide.deallocate(data, 0);
        }
        catch (const std::bad_array_new_length &)
        {
            overflow = true;
        }
        if (!overflow)
            return 2;

        // Fresh storage per allocation index includes payload, command record and callback slot growth.
        bool complete{};
        for (std::size_t index = 0; index != 32; ++index)
        {
            ResponseCallbackStore<> callbacks;
            RenderProgram<> packet;
            RenderProgramBuilder<> builder(packet, callbacks);
            builder.begin();
            auto owner = std::make_shared<int>(1);
            ReplyDispatchCallback callback([owner](ReplyPacketView, const ReplyRecord &) {});
            const RemoveViewPayload payload{{0, 1}, {0, 1}};
            lux_er1_client_allocation_fail_after(index);
            bool failed{};
            RequestId id = kInvalidRequestId;
            try
            {
                id = builder.pushWithReply(opcodes::CommandOp, type_ids::RemoveView, payload, std::move(callback));
            }
            catch (const std::bad_alloc &)
            {
                failed = true;
            }
            const auto attempts = lux_er1_client_allocation_disarm();
            callback = {};
            if (failed)
            {
                ++*failures;
                if (!attempts || callbacks.pendingCallbacks() || owner.use_count() != 1 || !packet.payload.empty() ||
                    !packet.commands.empty())
                    return 3;
                id = builder.pushWithReply(opcodes::CommandOp, type_ids::RemoveView, payload,
                                           ReplyDispatchCallback([owner](ReplyPacketView, const ReplyRecord &) {}));
            }
            if (id == kInvalidRequestId || callbacks.pendingCallbacks() != 1 || owner.use_count() != 2)
                return 4;
            lux_er1_client_allocation_fail_after(0);
            callbacks.cancel(id);
            const auto cancel_allocations = lux_er1_client_allocation_disarm();
            if (cancel_allocations || callbacks.pendingCallbacks() || owner.use_count() != 1)
                return 5;
            if (!failed)
            {
                complete = true;
                break;
            }
        }
        if (!complete)
            return 6;

        // The second deferred release fails after the first was published. Retry consumes only
        // the retained suffix, and the actual request ring contains each target exactly once.
        complete = false;
        bool partial{};
        for (std::size_t index = 0; index != 32; ++index)
        {
            auto channel = RenderControlChannel<>::create(8);
            auto sync = std::make_shared<RenderChannelSync>();
            RenderControlSession control(channel, sync);
            {
                auto first = control.adoptTarget({1, 1});
                auto second = control.adoptTarget({2, 1});
            }
            if (control.pendingTargetReleases() != 2)
                return 7;
            lux_er1_client_allocation_fail_after(index);
            const auto result = control.flushDeferredReleases();
            const auto attempts = lux_er1_client_allocation_disarm();
            if (!result)
            {
                ++*failures;
                if (!attempts || result.error().type != renderError<err::memory::OutOfMemory>().type)
                    return 8;
                partial |= control.pendingTargetReleases() == 1;
                const auto retried = control.flushDeferredReleases();
                if (!retried || !*retried)
                    return 9;
            }
            else if (!*result)
                return 10;
            OperationPacket<> packet;
            unsigned count{};
            std::uint32_t targets{};
            while (channel->requests.tryPop(packet) == lux::cxx::EQueuePopResult::VALUE)
            {
                DestroyTargetPayload payload{};
                std::memcpy(&payload, packet.payload.data() + packet.command.payload_offset, sizeof(payload));
                targets |= 1u << payload.target.index;
                ++count;
            }
            if (count != 2 || targets != 6 || control.pendingTargetReleases())
                return 11;
            if (result)
            {
                complete = true;
                break;
            }
        }
        if (!complete || !partial)
            return 12;

        auto channel = RenderProgramChannel<>::create(2);
        auto sync = std::make_shared<RenderChannelSync>();
        RenderProgramSession program(channel, sync);
        ProgramMemoryHints hints{};
        if (!program.beginFrame(hints))
            return 13;
        const std::int64_t origin[]{1, 2, 3};
        // Exhaust any existing payload capacity before arming the allocator, then record a new command.
        lux_er1_client_allocation_fail_after(0);
        const auto rebase = program.rebaseSceneOrigin({1, 1}, origin);
        const auto attempts = lux_er1_client_allocation_disarm();
        if (attempts && rebase)
            return 14;
        if (!rebase)
        {
            ++*failures;
            const auto retried = program.rebaseSceneOrigin({1, 1}, origin);
            if (!retried || !*retried)
                return 15;
        }
        return 0;
    }
    catch (const std::bad_alloc &)
    {
        lux_er1_client_allocation_disarm();
        return 100;
    }
}
