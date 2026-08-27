#pragma once
/**
 * @file ProgramEmitter.hpp
 * @brief Lightweight builder context for ExecutionProgram bytecode.
 *
 * ProgramEmitter wraps an ExecutionProgram& and provides the core emit()
 * helper used by all kernel codegen functions.  It lives in sinclude so
 * that kernel registration files (KernelDescriptor implementors) can use
 * it without pulling in the full RenderGraphCompiler translation unit.
 */

#include <cstdint>
#include <cstring>
#include <variant>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>

namespace lux::render
{
    /**
     * @brief Wraps ExecutionProgram& and provides emit() + resolveBufferSizeBytes().
     *
     * Kernel codegen helpers receive a ProgramEmitter& and call emit() to
     * append typed Command entries with packed argument data.  The emitter
     * handles offset bookkeeping automatically.
     */
    struct ProgramEmitter
    {
        ExecutionProgram& program;
        const RGCompiledGraph& compiled;

        /// Append a command with optional inline argument blob.
        /// @return  Command index (for DynamicPatch construction).
        uint32_t emit(ExecutionProgram::Command::EType type, const void* data, uint16_t data_size)
        {
            const uint32_t offset = static_cast<uint32_t>(program.command_data.size());
            if (data && data_size > 0)
            {
                const auto* bytes = static_cast<const std::byte*>(data);
                program.command_data.insert(program.command_data.end(), bytes, bytes + data_size);
            }
            program.commands.push_back({type, offset, data_size});
            return static_cast<uint32_t>(program.commands.size() - 1);
        }

        /// Resolve a buffer resource handle to its declared byte size.
        /// Returns 0 if the handle is invalid or the resource is not a buffer.
        VkDeviceSize resolveBufferSizeBytes(RGResourceHandle handle) const
        {
            if (!handle || handle.index >= compiled.original_graph.resources.size())
                return 0;
            const auto& res_desc = compiled.original_graph.resources[handle.index];
            if (res_desc.type != ERGResourceType::BUFFER)
                return 0;
            const auto* buf_desc = std::get_if<RGBufferDescription>(&res_desc.desc);
            if (buf_desc == nullptr)
                return 0;
            return static_cast<VkDeviceSize>(buf_desc->size);
        }

        /// Emit a kernel-specific sub-command (EType::KernelCommand).
        /// The command data is packed as: [KernelTypeId kernel_id, uint8_t sub_cmd, payload...].
        /// @return Command index (for DynamicPatch construction).
        uint32_t emitKernelCommand(KernelTypeId kernel_id, uint8_t sub_cmd, const void* payload, uint16_t payload_size)
        {
            // Header: [kernel_id(1 byte) | sub_cmd(1 byte) | payload_size(2 bytes)]
            struct Header
            {
                KernelTypeId kid;
                uint8_t sub;
                uint16_t psize;
            };
            static_assert(sizeof(Header) == 4);
            Header hdr{kernel_id, sub_cmd, payload_size};

            const uint16_t total = static_cast<uint16_t>(sizeof(Header) + payload_size);
            const uint32_t offset = static_cast<uint32_t>(program.command_data.size());

            // Append header
            const auto* hdr_bytes = reinterpret_cast<const std::byte*>(&hdr);
            program.command_data.insert(program.command_data.end(), hdr_bytes, hdr_bytes + sizeof(hdr));
            // Append payload
            if (payload && payload_size > 0)
            {
                const auto* p = static_cast<const std::byte*>(payload);
                program.command_data.insert(program.command_data.end(), p, p + payload_size);
            }

            program.commands.push_back({ExecutionProgram::Command::EType::KernelCommand, offset, total});
            return static_cast<uint32_t>(program.commands.size() - 1);
        }
    };

} // namespace lux::render
