#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/extensions/FrameDataMethod.hpp>
#include <lux/engine/render/extensions/detail/FrameDispatchContext.hpp>
#include <lux/engine/render/extensions/detail/HandlerRecord.hpp>
#include <lux/engine/render/extensions/storage/ResourceRegistry.hpp>
#include <stdexec/execution.hpp>

namespace lux::render
{
    enum class EFrameDataTarget : std::uint8_t
    {
        STORAGE,
        PARAMETERS,
        DRAW_LIST
    };

    // Stored in the real capability implementation. The Handler record has the sole object owner;
    // this record describes one frame-data method and borrows that stable registration.
    struct FrameDataCall final
    {
        cxx::type_info data_type;
        cxx::type_info handler_type;
        cxx::type_info method_type;
        std::size_t root_size = 0;
        std::size_t root_alignment = 1;
        EFrameDataTarget target;
        runtime_detail::HandlerRecord* handler = nullptr;
        RenderResult<void*> (*resolve_storage)(ResourceRegistry&, StorageId) noexcept = nullptr;
        void (*invoke)(void*, runtime_detail::FrameDispatchContext&) noexcept = nullptr;

        // Concrete implementation requirements. These are private bindings, not public payload schemas.
        std::uint32_t storage_dependency = UINT32_MAX;
        std::uint32_t graph_feature = UINT32_MAX;
        std::string_view contribution_key;
        std::string_view pass_key;

        bool operator==(const FrameDataCall& other) const noexcept
        {
            return data_type == other.data_type && handler_type == other.handler_type &&
                   method_type == other.method_type && target == other.target &&
                   storage_dependency == other.storage_dependency && graph_feature == other.graph_feature &&
                   contribution_key == other.contribution_key && pass_key == other.pass_key;
        }

        template <class T, auto Method> [[nodiscard]] static FrameDataCall declare() noexcept
        {
            using Description = FrameDataMethod<T, Method>;
            using Handler = typename Description::Handler;
            using Target = typename Description::Target;
            using Stored = typename FrameDataLayout<T>::Stored;
            using Result = std::invoke_result_t<decltype(Method), Handler&, Target, T>;
            constexpr bool synchronous = std::same_as<Result, RenderResult<void>>;
            static_assert(std::is_nothrow_invocable_v<decltype(Method), Handler&, Target, T>);
            static_assert(std::is_trivially_copyable_v<Stored> && std::is_trivially_destructible_v<Stored>);
            constexpr bool storage_target = std::is_lvalue_reference_v<Target> &&
                                            std::is_class_v<std::remove_reference_t<Target>> &&
                                            !std::is_const_v<std::remove_reference_t<Target>>;
            static_assert(
                storage_target || std::same_as<Target, FrameParameterTarget> || std::same_as<Target, FrameDrawTarget>
            );
            if constexpr (!synchronous)
            {
                using Environment =
                    stdexec::env<stdexec::prop<stdexec::get_scheduler_t, execution::RunLoop::Scheduler>>;
                static_assert(stdexec::sender<Result>);
                static_assert(std::same_as<
                              stdexec::value_types_of_t<Result, Environment, std::tuple, std::variant>,
                              std::variant<std::tuple<RenderResult<void>>>>);
            }
            FrameDataCall result;
            result.data_type = cxx::make_basic_type_info<T>();
            result.handler_type = cxx::make_basic_type_info<Handler>();
            result.method_type = cxx::make_basic_type_info<Description>();
            result.root_size = sizeof(Stored);
            result.root_alignment = alignof(Stored);
            if constexpr (storage_target)
            {
                result.target = EFrameDataTarget::STORAGE;
                result.resolve_storage = [](ResourceRegistry& resources, StorageId id) noexcept -> RenderResult<void*>
                {
                    const auto storage = resources.borrow<std::remove_reference_t<Target>>(id);
                    if (!storage)
                    {
                        return cxx::unexpected(storage.error());
                    }
                    return static_cast<void*>(*storage);
                };
            }
            else if constexpr (std::same_as<Target, FrameParameterTarget>)
            {
                result.target = EFrameDataTarget::PARAMETERS;
            }
            else
            {
                result.target = EFrameDataTarget::DRAW_LIST;
            }
            result.invoke = [](void* object, runtime_detail::FrameDispatchContext& context) noexcept
            {
                const auto& stored = *static_cast<const Stored*>(context.root);
                auto data = FrameDataLayout<T>::read(context.data, stored);
                if (!data)
                {
                    runtime_detail::completeFrameData(context.frame, context.entry, data.error());
                    return;
                }
                auto operation = [&]() noexcept -> Result
                {
                    if constexpr (storage_target)
                    {
                        auto& target = *static_cast<std::remove_reference_t<Target>*>(std::get<void*>(context.target));
                        return (static_cast<Handler*>(object)->*Method)(target, std::move(*data));
                    }
                    else
                    {
                        return (static_cast<Handler*>(object)->*Method)(
                            std::get<Target>(context.target),
                            std::move(*data)
                        );
                    }
                }();
                if constexpr (synchronous)
                {
                    auto& applied = operation;
                    runtime_detail::completeFrameData(
                        context.frame,
                        context.entry,
                        applied ? RenderError{} : applied.error()
                    );
                }
                else
                {
                    auto& frame = context.frame;
                    const auto entry = context.entry;
                    const auto scheduler = context.render_loop.scheduler();
                    auto normalized = std::move(operation) | stdexec::upon_error(Failure{}) |
                                      stdexec::upon_stopped(
                                          []() noexcept -> RenderResult<void>
                                          { return cxx::unexpected(RenderError{kCommandCancelled}); }
                                      ) |
                                      stdexec::continues_on(scheduler) |
                                      stdexec::then(
                                          [&frame, entry](RenderResult<void> result) noexcept
                                          {
                                              runtime_detail::completeFrameData(
                                                  frame,
                                                  entry,
                                                  result ? RenderError{} : result.error()
                                              );
                                          }
                                      );
                    stdexec::spawn(
                        std::move(normalized),
                        context.scope.get_token(),
                        stdexec::env{stdexec::prop{stdexec::get_scheduler, scheduler}}
                    );
                }
            };
            return result;
        }

    private:
        struct Failure final
        {
            RenderResult<void> operator()(RenderError error) const noexcept
            {
                return cxx::unexpected(error);
            }

            [[noreturn]] RenderResult<void> operator()(std::exception_ptr) const noexcept
            {
                std::terminate();
            }
        };
    };

    // One resolved input at an existing scope. Attachment and StorageSet lifetimes remain the owners;
    // neither this binding nor the frame's destination value extends a resource lifetime.
    struct BoundFrameData final
    {
        const FrameDataCall* call;
        VFrameDataTarget target;
    };
} // namespace lux::render
