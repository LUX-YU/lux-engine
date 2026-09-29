#pragma once
#include <lux/engine/editor/flowforge/FlowCompilationService.hpp>
#include <lux/engine/editor/EditorError.hpp>
// Private editor_flowforge source/status conversion. Removed with the old UI owner by P12.
namespace lux::editor::transition
{
    class FlowCompilationAccess final
    {
    public:
        static flowforge::FlowCompilationResult<flowforge::FlowCompileId> start(
            flowforge::FlowCompilationService& service,
            lux::flowforge::FlowSource source,
            sessions::ContentStamp stamp,
            sessions::ObservationVersion observed,
            lux::flowforge::FlowSourceEnvironment environment,
            std::filesystem::path linker
        )
        {
            return service.startSource(
                std::move(source),
                stamp,
                observed,
                flowforge::FlowCompileEnvironment{environment},
                {},
                {std::move(linker)}
            );
        }
        static EditorFailure failure(const flowforge::VFlowCompilationFailure& value)
        {
            return std::visit(
                [](const auto& error) -> EditorFailure {
                    using T = std::decay_t<decltype(error)>;
                    if constexpr (std::is_same_v<T, lux::flowforge::FlowForgeFailure>)
                        return {
                            EEditorError::SOURCE_FAILURE,
                            error.code == lux::flowforge::EFlowForgeError::LINK_FAILED ? "flowforge.link"
                                                                                       : "flowforge.compile",
                            static_cast<std::uint64_t>(error.code),
                            error.message,
                            error
                        };
                    else if constexpr (std::is_same_v<T, lux::flowforge::FlowSourceFailure>)
                        return {
                            EEditorError::SOURCE_FAILURE,
                            "flowforge.materialize",
                            static_cast<std::uint64_t>(error.code),
                            error.field,
                            error
                        };
                    else if constexpr (std::is_same_v<T, flowforge::EFlowCompilationError>)
                        return {
                            error == flowforge::EFlowCompilationError::BUSY ? EEditorError::BUSY
                                                                            : EEditorError::CANCELLED,
                            "flowforge.compile",
                            0,
                            {},
                            error
                        };
                    else
                        return {EEditorError::SOURCE_FAILURE, "flowforge.compile", 0, {}, error};
                },
                value
            );
        }
    };
}
namespace lux::editor::flowforge
{
    struct FlowEncoder final
    {
        EditorResult<lux::cxx::SharedBytes<>> operator()(
            const lux::flowforge::FlowSource& capture,
            std::stop_token stop
        ) const noexcept
        {
            if (stop.stop_requested())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "flowforge.encode"});
            }
            auto result = lux::flowforge::encodeFlowSource(capture);
            if (!result)
            {
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "flowforge.encode",
                    static_cast<std::uint64_t>(result.error().code),
                    result.error().field,
                    result.error()
                });
            }
            auto owner = std::make_shared<const std::string>(std::move(*result));
            return lux::cxx::SharedBytes<>::fromOwner(owner, std::as_bytes(std::span{owner->data(), owner->size()}));
        }
    };
}
