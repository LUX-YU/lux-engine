#pragma once
#include <cassert>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <string>

#if defined(_WIN32)
#define ED1_EXPORT __declspec(dllexport)
#else
#define ED1_EXPORT __attribute__((visibility("default")))
#endif
struct ConsumerReport final
{
    lux::editor::editing::HistoryId identity;
    std::size_t operations{}, plans{}, notices{};
    std::uint64_t checksum{};
};
struct ConsumerState
{
    std::size_t operations{}, plans{}, notices{};
    static void notice(void* object, const lux::editor::editing::HistoryNotice&) noexcept
    {
        ++static_cast<ConsumerState*>(object)->notices;
    }
};
extern "C" ConsumerReport runTextConsumer();
extern "C" ConsumerReport runRecordsConsumer();
