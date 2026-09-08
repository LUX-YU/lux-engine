#pragma once
#include <cassert>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <string>

#if defined(_WIN32)
#if defined(editing_consumer_Text_EXPORTS)
#define ED1_TEXT_API __declspec(dllexport)
#else
#define ED1_TEXT_API __declspec(dllimport)
#endif
#if defined(editing_consumer_Records_EXPORTS)
#define ED1_RECORDS_API __declspec(dllexport)
#else
#define ED1_RECORDS_API __declspec(dllimport)
#endif
#else
#define ED1_TEXT_API __attribute__((visibility("default")))
#define ED1_RECORDS_API __attribute__((visibility("default")))
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
extern "C" ED1_TEXT_API ConsumerReport runTextConsumer();
extern "C" ED1_RECORDS_API ConsumerReport runRecordsConsumer();
