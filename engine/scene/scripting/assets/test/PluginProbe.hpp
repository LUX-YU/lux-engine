#pragma once

#include <lux/engine/scene/scripting/ScriptAssetAccess.hpp>

#include <atomic>
#include <thread>

struct PluginCounts final
{
    const std::thread::id owner{std::this_thread::get_id()};
    std::atomic<unsigned> decoded{};
    std::atomic<unsigned> destroyed{};
    std::atomic<bool> wrong_decode_thread{};
    std::atomic<bool> wrong_destroy_thread{};
    unsigned unloaded{};
};

using StartPluginRead = bool(
    lux::scene::script::ScriptAssetScope&,
    lux::asset::AssetId,
    lux::scene::script::ScriptAssetScope::Completion,
    std::shared_ptr<const void>,
    PluginCounts&
) noexcept;
