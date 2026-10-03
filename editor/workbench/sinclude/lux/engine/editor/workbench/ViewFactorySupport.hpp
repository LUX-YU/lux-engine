#pragma once
#include <lux/engine/editor/views/ViewFactory.hpp>
#include <algorithm>
#include <concepts>
#include <type_traits>
#include <variant>

namespace lux::editor::workbench::detail
{
    template <class Error> views::ViewFactoryFailure viewFailure(const Error& error)
    {
        if constexpr (std::same_as<Error, views::ViewFactoryFailure>)
            return error;
        else if constexpr (requires { error.index(); })
            return std::visit([](const auto& value) { return viewFailure(value); }, error);
        else if constexpr (requires { error.cause; })
            return viewFailure(error.cause);
        else
        {
            views::ViewFactoryFailure result{
                views::EViewFactoryError::CONSTRUCT,
                std::string(cxx::typeToken<Error>().name())
            };
            if constexpr (requires { error.retryable; })
                if (error.retryable)
                    result.code = views::EViewFactoryError::BUSY;
            if constexpr (requires { error.session; error.code == decltype(error.code)::SESSION; })
                if (error.code == decltype(error.code)::SESSION)
                    return viewFailure(error.session);
            if constexpr (requires { error == Error::BUSY; })
                if (error == Error::BUSY)
                    result.code = views::EViewFactoryError::BUSY;
            if constexpr (std::is_enum_v<Error>)
                result.domain_code = static_cast<std::uint64_t>(error);
            else if constexpr (requires { error.code; })
                result.domain_code = static_cast<std::uint64_t>(error.code);
            if constexpr (std::is_convertible_v<Error, std::string_view>)
                result.detail = std::string_view(error);
            else if constexpr (requires { std::string{error.message}; })
                result.detail = error.message;
            else if constexpr (requires {
                                   error.message.data();
                                   error.message.size();
                               })
            {
                const auto end = std::find(error.message.begin(), error.message.end(), '\0');
                result.detail.assign(error.message.begin(), end);
            }
            return result;
        }
    }
    template <class View, class Payload>
    views::ViewFactoryResult<void> connectIntent(
        views::DetachedView& view,
        object::TSignal<Payload> View::* signal,
        const std::shared_ptr<cxx::move_only_function<void(const Payload&)>>& receiver
    )
    {
        if (!*receiver)
            return {};
        // The factory just constructed this exact view type; no runtime type probing is needed.
        auto connection = object::LuxObject::connect(
            static_cast<View*>(view.pane()), signal,
            [receiver](const Payload& value) noexcept { (*receiver)(value); }
        );
        if (!connection)
            return cxx::unexpected(viewFailure(connection.error()));
        view.addConnection(std::move(*connection));
        return {};
    }
    template <const views::ViewFactoryDescriptor& Descriptor, class Input, class Create>
        requires requires(Create& create, const views::ViewFactoryInput& input, const Input& value) {
            create(input, value);
        }
    std::shared_ptr<views::ViewFactoryEntry> bindViewFactory(Create create)
    {
        static_assert(Descriptor.binding_type == cxx::typeToken<Input>());
        return views::ViewFactoryEntry::bind<Descriptor>(
            contracts::CodeLease::builtin(),
            [create = std::move(create)](const views::ViewFactoryInput& input) mutable
                -> views::ViewFactoryResult<views::DetachedView> {
                auto view = create(input, *static_cast<const Input*>(input.binding()));
                if (!view)
                    return cxx::unexpected(viewFailure(view.error()));
                return std::move(*view);
            }
        );
    }
}
