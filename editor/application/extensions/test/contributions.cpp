#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/ui/Root.hpp>
#include <cassert>
#include <iostream>

using namespace lux;
using namespace lux::editor;
using namespace lux::editor::extensions;
namespace
{
    template <class T> auto take(T value)
    {
        assert(value);
        return std::move(*value);
    }
    struct Binding final
    {
        unsigned value;
    };
    struct Facts final
    {
        bool code_alive{true};
        unsigned destroyed{}, old_calls{}, new_calls{};
    };
    class Window final : public ui::Pane
    {
    public:
        Window(object::ObjectDispatcherRef dispatcher, ui::PaneId id, Facts& facts)
            : Pane(dispatcher, std::move(id), ui::PaneTypeId{"extension.window"}, "Extension"), facts_(facts)
        {}
        ~Window() override
        {
            assert(facts_.code_alive);
            ++facts_.destroyed;
        }

    private:
        Facts& facts_;
    };
}
int main()
{
    auto messages = take(object::ObjectMessageQueue::create(32));
    commands::CommandRegistry commands;
    ContributionRegistry registry{messages.dispatcherRef(), commands, 2};
    Facts facts;
    auto library = std::shared_ptr<const void>(new int{1}, [&](const void* p) {
        assert(facts.destroyed == 2);
        facts.code_alive = false;
        delete static_cast<const int*>(p);
        auto recursive = registry.applyPending();
        assert(!recursive && recursive.error().code == EContributionError::BUSY);
    });
    auto code = contracts::CodeLease::plugin(library);
    auto empty = take(ContributionSnapshot::prepare({}));
    auto later = empty;
    auto pending = empty;
    ContributionDraft draft;
    bool replace{};
    draft.views.push_back(std::make_shared<views::ViewFactoryEntry>(
        code,
        views::ViewFactoryDescriptor{views::ViewTypeId{"extension.window"}, "Window", cxx::typeToken<Binding>()},
        [&, code](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
            ++facts.old_calls;
            if (!replace)
            {
                replace = true;
                assert(registry.enqueue(later));
                const auto refused = registry.applyPending();
                assert(!refused && refused.error().code == EContributionError::BUSY);
                assert(registry.revision() == 1);
            }
            return views::DetachedView{code, std::make_unique<Window>(input.dispatcher(), input.paneId(), facts)};
        }
    ));
    auto prepared = take(ContributionSnapshot::prepare(std::move(draft)));
    assert(registry.enqueue(prepared));
    assert(!prepared.valid());
    assert(registry.applyPending());
    std::vector<views::DetachedView> views;
    auto batch = [&](const ContributionSnapshot& snapshot) -> ContributionResult<void> {
        for (const auto id : {"one", "two"})
        {
            views::ViewFactoryInput input{
                messages.dispatcherRef(),
                ui::PaneId{id},
                contracts::CodeLease::builtin(),
                cxx::typeToken<Binding>(),
                std::make_shared<const Binding>(Binding{9})
            };
            views.push_back(take(snapshot.views().prepare(views::ViewTypeId{"extension.window"}, input)));
            assert(!views.back().pane()->attachedRoot());
        }
        assert(snapshot.views().entries().size() == 1 && facts.old_calls == 2);
        return {};
    };
    assert(registry.withSnapshot(batch));
    assert(registry.enqueue(pending));
    auto full = empty;
    auto refused = registry.enqueue(full);
    assert(!refused && full.valid());
    library.reset();
    code = contracts::CodeLease::builtin();
    views.clear();
    assert(facts.destroyed == 2 && facts.code_alive);
    // The external pin retires under the publication guard, after all catalogs have changed.
    assert(registry.applyPending());
    assert(!facts.code_alive);
    assert(registry.snapshot().views().entries().empty() && commands.snapshot().entries().empty());
    assert(registry.applyPending());

    // Notification runs only after publication. A nested request is retained for the next outer turn.
    unsigned notifications{};
    object::LuxObject receiver{messages.dispatcherRef()};
    auto notification = take(object::LuxObject::connect(
        &registry,
        &ContributionRegistry::changed,
        &receiver,
        [&](std::uint64_t value) noexcept {
            ++notifications;
            assert(value == registry.revision());
            assert(commands.revision() == value);
            assert(!registry.applyPending());
            if (notifications == 1)
            {
                auto next = empty;
                assert(registry.enqueue(next));
            }
        },
        object::EDelivery::DIRECT
    ));
    auto next = empty;
    assert(registry.enqueue(next));
    assert(registry.applyPending());
    assert(notifications == 1);
    assert(registry.applyPending());
    assert(notifications == 2);

    // Duplicate type and incomplete factories reject a complete candidate, leaving the live revision untouched.
    const auto revision = registry.revision();
    ContributionDraft broken;
    broken.sessions.push_back(nullptr);
    assert(!ContributionSnapshot::prepare(std::move(broken)));
    assert(registry.revision() == revision);
    std::cout
        << "PASS immutable batch, bounded publication, cleanup pins, post-publication notification and rejection\n";
}
