#include <lux/engine/flowforge/Compiler.hpp>

#include <lux/engine/flowforge/compiler/AOT.hpp>
#include <lux/engine/flowforge/compiler/IR.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/script/ScriptGraph.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>

#include <atomic>
#include <fstream>
#include <limits>
#include <new>
#include <system_error>

namespace lux::flowforge
{
    namespace
    {
        class TemporaryCompileDirectory final
        {
        public:
            TemporaryCompileDirectory() = default;

            explicit TemporaryCompileDirectory(std::filesystem::path path) noexcept
                : path_(std::move(path))
            {
            }

            ~TemporaryCompileDirectory()
            {
                std::error_code error;
                std::filesystem::remove_all(path_, error);
            }

            TemporaryCompileDirectory(const TemporaryCompileDirectory&) = delete;
            TemporaryCompileDirectory& operator=(const TemporaryCompileDirectory&) = delete;
            TemporaryCompileDirectory(TemporaryCompileDirectory&&) noexcept = default;
            TemporaryCompileDirectory& operator=(TemporaryCompileDirectory&&) noexcept = default;

            [[nodiscard]] const std::filesystem::path& path() const noexcept
            {
                return path_;
            }

        private:
            std::filesystem::path path_;
        };

        [[nodiscard]] FlowForgeResult<TemporaryCompileDirectory> createTemporaryDirectory() noexcept
        {
            try
            {
                std::error_code error;
                const auto root = std::filesystem::temp_directory_path(error);
                if (error)
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::IO_FAILED,
                        .message = "cannot resolve the temporary directory"
                    });
                }

                static std::atomic<std::uint64_t> next_id{1U};
                for (std::uint32_t attempt{}; attempt < 64U; ++attempt)
                {
                    const auto id = next_id.fetch_add(1U, std::memory_order_relaxed);
                    auto path = root / ("lux-flowforge-" + std::to_string(id));
                    error.clear();
                    if (std::filesystem::create_directory(path, error))
                    {
                        return TemporaryCompileDirectory(std::move(path));
                    }
                }
                return lux::cxx::unexpected(FlowForgeFailure{
                    .code = EFlowForgeError::IO_FAILED,
                    .message = "cannot create a unique FlowForge compile directory"
                });
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
            }
            catch (...)
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::FOREIGN_EXCEPTION});
            }
        }

        [[nodiscard]] FlowForgeResult<std::vector<std::byte>> readModule(
            const std::filesystem::path& path
        ) noexcept
        {
            try
            {
                std::ifstream stream(path, std::ios::binary | std::ios::ate);
                if (!stream)
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::IO_FAILED,
                        .message = "cannot open the linked FlowForge module"
                    });
                }
                const auto end = stream.tellg();
                if (end <= 0)
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::IO_FAILED,
                        .message = "the linked FlowForge module is empty"
                    });
                }
                std::vector<std::byte> bytes(static_cast<std::size_t>(end));
                stream.seekg(0, std::ios::beg);
                stream.read(reinterpret_cast<char*>(bytes.data()), end);
                if (!stream)
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::IO_FAILED,
                        .message = "cannot read the linked FlowForge module"
                    });
                }
                return bytes;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
            }
            catch (...)
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::FOREIGN_EXCEPTION});
            }
        }
    }

    FlowForgeResult<lux::script::ScriptArtifact> compileFlowForgeScript(
        const FlowGraph& graph,
        FlowForgeCompileOptions options
    ) noexcept
    {
        try
        {
            if (options.module_name.empty())
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::INVALID_MODULE_NAME});
            }
            if (graph.exports().empty() || !validFlowForgeExports(graph))
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::GRAPH_INVALID});
            }

            auto context = IRContext::create();
            if (!context)
            {
                return lux::cxx::unexpected(std::move(context.error()));
            }
            auto object = compileToObject(*context, graph, options);
            if (!object)
            {
                return lux::cxx::unexpected(std::move(object.error()));
            }
            const bool is_invalid_state_size = object->state_size > std::numeric_limits<std::uint32_t>::max();
            const bool is_invalid_state_defaults = object->state_defaults.size() > object->state_size;
            if (is_invalid_state_size || is_invalid_state_defaults)
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::INVALID_DESCRIPTION});
            }

            auto temporary = createTemporaryDirectory();
            if (!temporary)
            {
                return lux::cxx::unexpected(std::move(temporary.error()));
            }
            const auto module_path = temporary->path() / "flowforge-script.dll";
            auto linked = linkSharedLibrary(*object, module_path, options);
            if (!linked)
            {
                return lux::cxx::unexpected(std::move(linked.error()));
            }
            auto payload = readModule(module_path);
            if (!payload)
            {
                return lux::cxx::unexpected(std::move(payload.error()));
            }

            lux::rdesc::Script description;
            description.schema_version = lux::rdesc::Script::kSchemaVersion;
            description.module_name = options.module_name;
            description.exports = std::move(object->exports);
            description.lifecycle = options.lifecycle;
            description.body = lux::rdesc::NativeModuleScript{
                LUX_SCRIPT_ABI_VERSION,
                object->state_hash,
                static_cast<std::uint32_t>(object->state_size),
                object->state_align,
                std::move(object->state_defaults)
            };

            auto artifact = lux::script::ScriptArtifact::create(std::move(description), std::move(*payload));
            if (!artifact)
            {
                const auto code = artifact.error() == lux::script::EScriptArtifactError::ALLOCATION_FAILURE
                    ? EFlowForgeError::ALLOCATION_FAILURE
                    : EFlowForgeError::INVALID_DESCRIPTION;
                return lux::cxx::unexpected(FlowForgeFailure{.code = code});
            }
            return std::move(*artifact);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
        }
        catch (...)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::FOREIGN_EXCEPTION});
        }
    }
}
