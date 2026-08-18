#pragma once
/**
 * @file FlowGraphCompiler.hpp
 * @brief Main-thread coordinator for FlowGraph ahead-of-time compilation.
 *
 * A graph asset is immutable-snapshotted before it enters the executor. The
 * compiler owns cache admission, duplicate suppression and result adoption;
 * CPU scheduling belongs to the injected FlowGraphCompileClient.
 */

#include "flow/FlowGraphCompile.hpp"

#include <lux/engine/resource/asset/Asset.hpp>

#include <filesystem>
#include <memory>

namespace lux::asset { class AssetManager; }

namespace lux::editor
{
    class FlowGraphCompiler final
    {
    public:
        explicit FlowGraphCompiler(
            std::filesystem::path cache_dir,
            FlowGraphCompileClient compile_client);
        ~FlowGraphCompiler();

        FlowGraphCompiler(const FlowGraphCompiler&) = delete;
        FlowGraphCompiler& operator=(const FlowGraphCompiler&) = delete;

        void setCacheDir(std::filesystem::path dir);

        /// Resolve, snapshot and enqueue one graph asset on a cache miss.
        void precompile(
            lux::asset::AssetManager& assets,
            const lux::asset::asset_id_t& id);

    private:
        friend FlowGraphCompileResult compileFlowGraphJob(
            const FlowGraphCompileJob& job) noexcept;

        struct Impl;
        // The compiler is the sole strong owner. Completion callbacks keep only
        // weak observers, so a late main-thread delivery cannot resurrect or
        // dereference a destroyed compiler.
        std::shared_ptr<Impl> impl_;
    };
}
