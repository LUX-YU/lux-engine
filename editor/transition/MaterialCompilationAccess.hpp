#pragma once
#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/editor/EditorError.hpp>
// Private P07 conversion for editor_material; delete with its legacy source owner by P12.
namespace lux::editor::transition
{
    class MaterialCompilationAccess final
    {
    public:
        static material::MaterialCompileResult<std::unique_ptr<material::MaterialCompileOperation>> start(
            process::ExecutionRuntime& execution,
            lux::material::MaterialSource source,
            material::MaterialCompileKey key,
            sessions::ObservationVersion observed
        )
        {
            return material::MaterialCompileOperation::startSource(
                execution,
                std::make_shared<const lux::material::MaterialSource>(std::move(source)),
                key,
                observed
            );
        }
        static EditorFailure failure(const material::VMaterialCompileFailure& failure)
        {
            return std::visit(
                [](const auto& error) -> EditorFailure {
                    using T = std::decay_t<decltype(error)>;
                    if constexpr (std::is_same_v<T, lux::material::MaterialCompileFailure>)
                        return {
                            EEditorError::SOURCE_FAILURE,
                            "material.compile",
                            static_cast<std::uint64_t>(error.code),
                            error.message,
                            error
                        };
                    else if constexpr (std::is_same_v<T, material::EMaterialCompileRequestError>)
                        return {
                            error == material::EMaterialCompileRequestError::BUSY ? EEditorError::BUSY
                                                                                  : EEditorError::CANCELLED,
                            "material.compile",
                            0,
                            {},
                            error
                        };
                    else
                        return {EEditorError::SOURCE_FAILURE, "material.compile", 0, {}, error};
                },
                failure
            );
        }
    };
}
namespace lux::editor::material
{
    struct MaterialEncoder final
    {
        EditorResult<lux::cxx::SharedBytes<>> operator()(
            const lux::material::MaterialSource& capture,
            std::stop_token stop
        ) const noexcept
        {
            if (stop.stop_requested())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "material.encode"});
            }
            auto result = lux::material::encodeMaterialSource(capture);
            if (!result)
            {
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "material.encode",
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
