#pragma once

#include <concepts>
#include <exec/task.hpp>
#include <lux/engine/process/asset_loading/AssetRead.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <stop_token>
#include <utility>

namespace lux::engine::render
{
    struct RenderAssetVersion final
    {
        asset::AssetId asset;
        std::uint64_t revision = 0;
        std::uint64_t source = 0;

        friend bool operator==(const RenderAssetVersion&, const RenderAssetVersion&) noexcept = default;

        friend bool operator<(const RenderAssetVersion& left, const RenderAssetVersion& right) noexcept
        {
            if (left.source != right.source)
            {
                return left.source < right.source;
            }
            return left.asset < right.asset || (left.asset == right.asset && left.revision < right.revision);
        }
    };

    struct RenderAssetLimits final
    {
        std::size_t in_flight = 8;
        asset::AssetDecodeLimits decode{64 * 1024 * 1024, 128 * 1024 * 1024, 32};
    };

    // A finite load owns its source snapshot. Only immutable values cross the CPU/Asset threads.
    struct RenderAssetLoad final
    {
        RenderAssetVersion version;
        process::asset_loading::AssetReadPort source;
        asset::AssetVfsView snapshot;
        asset::AssetDecodeLimits limits;
        std::stop_token stop;
    };

    // Pipelines define the actual representation and its logical release. The cache handles demand,
    // admission and Main delivery; it never needs to know which resource family a pipeline produces.
    template <class T>
    concept RenderAssetPipeline = requires(T& pipeline, RenderAssetLoad load, typename T::Value& value) {
        { T::name } -> std::convertible_to<std::string_view>;
        {
            pipeline.load(std::move(load))
        } noexcept -> std::same_as<exec::task<lux::render::RenderResult<typename T::Value>>>;
        { pipeline.release(value) } noexcept -> std::same_as<exec::task<lux::render::RenderResult<void>>>;
        // Equal cache destinations may share a table only when all representation dependencies agree.
        { std::as_const(pipeline).compatible(std::as_const(pipeline)) } noexcept -> std::same_as<bool>;
    };
} // namespace lux::engine::render
