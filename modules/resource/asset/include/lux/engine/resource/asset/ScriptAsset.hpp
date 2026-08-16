#pragma once
/**
 * @file ScriptAsset.hpp
 * @brief Asset wrapper for a script artefact (`.luxasset` containing description + payload).
 *
 * `ScriptAsset` bundles two things, kept strictly separate:
 *   - the *description* `lux::rdesc::Script` (kind, ABI version, function
 *     table, dependencies, provenance) — pure metadata, no bytes;
 *   - the *payload* bytes whose semantics are dictated by `Script::kind`:
 *       LuaSource     → UTF-8 source
 *       LuaBytecode   → LuaJIT bytecode
 *       NativeLibrary → compiled shared-library bytes
 *       PythonSource  → UTF-8 source
 *
 * The on-disk `.luxasset` layout (`AssetFileHeader` + binary description
 * blob + payload) is the concern of @ref ScriptSerDeser. The asset layer
 * never `dlopen`s anything — that's `lux::script::ScriptHost`'s job.
 */

#include <cstddef>
#include <utility>
#include <vector>

#include <lux/engine/description/Script.hpp>

#include "Asset.hpp"
#include "AssetSerDeser.hpp"

namespace lux::asset
{
    class LUX_RESOURCE_PUBLIC ScriptAsset : public TAsset<lux::rdesc::Script>
    {
    public:
        static constexpr EAssetType asset_type{ EAssetType::SCRIPT };

        explicit ScriptAsset(std::unique_ptr<AssetInfo> info,
                             std::unique_ptr<lux::rdesc::Script> script = nullptr,
                             std::vector<std::byte>     payload = {});

        /// Raw payload; meaning depends on `description().kind`.
        const std::vector<std::byte>& payload() const noexcept { return payload_; }
        std::vector<std::byte>&       payload()       noexcept { return payload_; }

        void setPayload(std::vector<std::byte> bytes) noexcept { payload_ = std::move(bytes); }

    private:
        std::vector<std::byte> payload_;
    };
}
