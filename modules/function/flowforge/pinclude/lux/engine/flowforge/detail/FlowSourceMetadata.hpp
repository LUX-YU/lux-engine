#pragma once

#include <lux/engine/flowforge/graph/FlowSource.hpp>

namespace lux::flowforge::detail
{
    [[nodiscard]] const meta::RefType* findSourceType(std::string_view, const FlowSourceEnvironment&) noexcept;
    [[nodiscard]] const meta::RefClass* findSourceClass(std::string_view, const FlowSourceEnvironment&) noexcept;
    [[nodiscard]] std::vector<FlowSourceArgument> captureSourceArguments(const std::vector<FuncArgInfo>&);
    [[nodiscard]] FlowSourceResult<std::vector<FuncArgInfo>>
    restoreSourceArguments(const std::vector<FlowSourceArgument>&, const FlowSourceEnvironment&) noexcept;

    inline auto sourceFailure(EFlowSourceError error, std::string field = {}, NodeId node = {}) noexcept
    {
        return cxx::unexpected(FlowSourceFailure{error, std::move(field), node});
    }

    inline auto sourceCodecFailure(FlowForgeFailure cause, std::string field = {}, NodeId node = {}) noexcept
    {
        FlowSourceFailure error{EFlowSourceError::NODE_CODEC_FAILURE, std::move(field), node};
        error.cause = std::move(cause);
        return cxx::unexpected(std::move(error));
    }

    template <class T, auto Clone, class... Args>
    FlowSourceResult<FlowNodePayload> sourcePayload(const object::CodeLease& code, Args&&... args) noexcept
    {
        auto payload = FlowNodePayload::make<T, Clone>(code, std::forward<Args>(args)...);
        if (!payload)
        {
            return sourceCodecFailure(std::move(payload.error()));
        }
        return std::move(*payload);
    }
} // namespace lux::flowforge::detail
