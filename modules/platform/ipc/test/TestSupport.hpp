#pragma once
#include "Allocation.hpp"
#include "TestHooks.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif
namespace ipc_test
{
    using namespace lux::platform::ipc;
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    inline void check(bool condition, const char* message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "FAILED: %s\n", message);
            std::abort();
        }
    }

    template <typename T> T must(Result<T> result)
    {
        check(static_cast<bool>(result), "expected success");
        return std::move(*result);
    }

    inline void must(bool result)
    {
        check(result, "expected success");
    }

    inline void must(Result<void> result)
    {
        check(static_cast<bool>(result), "expected success");
    }

    template <std::size_t Size> struct FixedValue final
    {
        static_assert(Size >= 8 && Size % 8 == 0);
        std::array<std::uint64_t, Size / 8> words{};

        explicit FixedValue(std::uint64_t value) noexcept
        {
            words[0] = value;
        }
    };

    template <std::size_t Size = 8> IpcMessage makeValue(std::uint64_t value)
    {
        IpcMessage message;
        message.emplace<FixedValue<Size>>(value);
        return message;
    }

    template <std::size_t Size = 8> std::uint64_t valueOf(const IpcMessage& message)
    {
        const auto& value = message.get<FixedValue<Size>>();
        for (std::size_t i = 1; i < value.words.size(); ++i)
        {
            check(value.words[i] == 0, "validate every fixed array field");
        }
        return value.words[0];
    }

    inline ReceivedRequest receive(IpcResponder& responder)
    {
        for (int i = 0; i < 128; ++i)
        {
            ReceivedRequest result;
            if (responder.tryReceive(result) == EIpcReceive::RECEIVED)
            {
                return result;
            }
        }
        check(false, "request available");
        return {};
    }

    inline EIpcReceive receiveReply(IpcRequester& requester, IpcMessage& output)
    {
        std::uint64_t correlation = 0;
        const auto status = requester.tryReceive(correlation, output);
        if (status == EIpcReceive::RECEIVED)
        {
            check(correlation == 0, "generic queue correlation");
        }
        return status;
    }

    inline IpcMessage receivedReply(IpcRequester& requester, std::uint64_t* correlation = nullptr)
    {
        IpcMessage output;
        std::uint64_t received = 0;
        check(requester.tryReceive(received, output) == EIpcReceive::RECEIVED, "reply available");
        if (correlation)
        {
            *correlation = received;
        }
        else
        {
            check(received == 0, "generic reply correlation");
        }
        return output;
    }

    inline void publish(IpcResponder& responder, ReceivedRequest& work, IpcMessage& result)
    {
        const auto deadline = Clock::now() + 10s;
        for (;;)
        {
            const auto sent = responder.trySendResponse(work.requesterId(), work.correlation(), result);
            if (sent)
            {
                work.reset();
                return;
            }
            check(sent.error() == EIpcError::WOULD_BLOCK && Clock::now() < deadline, "reply retry timed out");
            std::this_thread::yield();
        }
    }

    inline void drain(IpcRequester& requester)
    {
        IpcMessage reply;
        while (receiveReply(requester, reply) == EIpcReceive::RECEIVED)
        {
            reply = {};
        }
    }

    inline void empty()
    {
        auto counts = testing::counts();
        check(
            counts.channels == 0 && counts.links == 0 && counts.rings == 0 && counts.segments == 0,
            "all physical storage reclaimed"
        );
    }

#ifdef _WIN32
    inline std::array<DWORD_PTR, 2> physicalCores()
    {
        DWORD size = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
        std::vector<std::byte> storage(size);
        check(
            GetLogicalProcessorInformationEx(
                RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data()),
                &size
            ),
            "CPU topology"
        );
        DWORD_PTR process_mask = 0, system_mask = 0;
        check(GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask), "process CPU mask");
        std::array<DWORD_PTR, 2> result{};
        std::size_t count = 0;
        for (std::size_t offset = 0; offset < size && count < result.size();)
        {
            auto* item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data() + offset);
            if (item->Processor.GroupMask[0].Group == 0)
            {
                const auto mask = item->Processor.GroupMask[0].Mask & process_mask;
                if (mask)
                {
                    result[count++] = mask & (~mask + 1);
                }
            }
            offset += item->Size;
        }
        check(count == 2, "two separate physical cores available");
        std::printf("physical_core_masks=%llu,%llu\n", result[0], result[1]);
        return result;
    }
#endif

    struct Owner
    {
        std::vector<std::byte> bytes;
        int* destroyed;

        Owner(std::size_t size, int* count) : bytes(size), destroyed(count) {}

        ~Owner()
        {
            ++*destroyed;
        }
    };

} // namespace ipc_test
