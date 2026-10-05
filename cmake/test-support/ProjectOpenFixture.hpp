#pragma once
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>

namespace lux::test
{
    inline services::ServiceResult<std::unique_ptr<editor::project::ProjectView::Open>>
    createProjectOpen(services::ServiceResolver&, const services::ServiceConfiguration&) noexcept
    {
        return std::make_unique<editor::project::ProjectView::Open>();
    }
    inline std::shared_ptr<const services::ServiceEntry> projectOpenFixture()
    {
        using Open = editor::project::ProjectView::Open;
        static constexpr services::ServiceContract contracts[]{
            services::ServiceContract::forType<Open, Open>(services::ServiceNameView{"lux.editor.project.open"})
        };
        static constexpr auto descriptor = services::ServiceDescriptor::forType<Open, createProjectOpen>(
            services::ServiceNameView{"test.project.open"}, contracts
        );
        return services::ServiceEntry::bind<descriptor>(object::CodeLease::builtin());
    }
} // namespace lux::test
