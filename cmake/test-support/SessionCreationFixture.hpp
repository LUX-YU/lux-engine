#pragma once
#include <functional>
#include <lux/engine/editor/sessions/SessionCommands.hpp>
#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>

namespace lux::test
{
    // Test-only headless receiver. It executes the original real SessionPreparation and Store
    // supplied by each scenario, through the same shared contracts as the workbench owner.
    struct SessionCreationFixture final
    {
        using Create = std::function<
            editor::commands::CommandResult<editor::commands::DispatchReceipt>(editor::sessions::SessionPreparation)>;
        using Query = std::function<
            editor::commands::CommandResult<editor::commands::CommandState>(const editor::commands::CommandQuery&)>;
        struct Input final
        {
            Create create;
            Query query;
        };
        editor::sessions::SessionCreation create_;
        editor::commands::CommandEntry::Query query_;
        explicit SessionCreationFixture(const Input& input) : create_(input.create), query_(input.query) {}
        SessionCreationFixture(const SessionCreationFixture&) = delete;
        SessionCreationFixture& operator=(const SessionCreationFixture&) = delete;
        static services::ServiceResult<std::unique_ptr<SessionCreationFixture>>
        create(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto input = resolver.definition<Input>();
            if (!input)
            {
                return cxx::unexpected(std::move(input.error()));
            }
            return std::make_unique<SessionCreationFixture>(**input);
        }
        static std::shared_ptr<const services::ServiceEntry> entry(Create create, Query query = {})
        {
            if (!query)
            {
                query = [](const editor::commands::CommandQuery&)
                    -> editor::commands::CommandResult<editor::commands::CommandState>
                { return editor::commands::CommandState{true}; };
            }
            static constexpr services::ServiceContract contracts[]{
                {editor::sessions::kSessionCreation,
                 1,
                 cxx::typeToken<editor::sessions::SessionCreation>(),
                 [](void* value) noexcept -> void* { return &static_cast<SessionCreationFixture*>(value)->create_; }},
                {editor::sessions::kSessionCreationAvailability,
                 1,
                 cxx::typeToken<editor::commands::CommandEntry::Query>(),
                 [](void* value) noexcept -> void* { return &static_cast<SessionCreationFixture*>(value)->query_; }}
            };
            static constexpr auto descriptor = []
            {
                auto result =
                    services::ServiceDescriptor::forType<SessionCreationFixture, &SessionCreationFixture::create>(
                        services::ServiceNameView{"test.session.creation"},
                        contracts
                    );
                result.definition_type = cxx::typeToken<Input>();
                return result;
            }();
            return services::ServiceEntry::bind<descriptor>(
                object::CodeLease::builtin(),
                std::make_shared<const Input>(Input{std::move(create), std::move(query)})
            );
        }
    };
} // namespace lux::test
