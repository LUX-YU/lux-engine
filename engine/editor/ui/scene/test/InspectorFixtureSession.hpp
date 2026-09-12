#pragma once
#include "InspectorFixture.hpp"
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <cassert>
namespace inspector_test
{
    namespace ed = lux::editor::editing;
    class FixtureSession final
    {
        std::unique_ptr<inspector_fixture::Component> value_{std::make_unique<inspector_fixture::Component>()};
        struct Operation final : ed::EditOperation
        {
            FixtureSession &owner;
            inspector_fixture::Component before, after;
            ed::StateId base;
            Operation(FixtureSession &session, const inspector_fixture::Component &next)
                : owner(session), before(*session.value_), after(next),
                  base(session.history->view()->snapshot.current) {}
            ed::HistoryId historyId() const noexcept override { return base.history; }
            ed::StateId baseState() const noexcept override { return base; }
            std::string_view label() const noexcept override { return "Generated Inspector change"; }
            std::size_t retainedBytesUpperBound() const noexcept override { return 65536; }
            struct Plan final : ed::PreparedEdit
            {
                FixtureSession &owner;
                std::unique_ptr<inspector_fixture::Component> next;
                bool changed;
                Plan(FixtureSession &session, const inspector_fixture::Component &value)
                    : owner(session), next(std::make_unique<inspector_fixture::Component>(value)),
                      changed(*next != *session.value_) {}
                ed::EEditEffect effect() const noexcept override
                {
                    return changed ? ed::EEditEffect::CHANGE : ed::EEditEffect::NO_CHANGE;
                }
                void apply() noexcept override
                {
                    owner.value_.swap(next);
                }
                void publish(const ed::CommitInfo &info) noexcept override
                {
                    assert(owner.history->view()->snapshot.current == info.to);
                    ++owner.publishes;
                }
            };
            ed::EditResult<ed::PreparedEditPtr> prepare(const ed::ApplyContext &context,
                                                       ed::EditPreparationBudget &budget) const noexcept override
            {
                if (owner.reject)
                    return lux::cxx::unexpected(ed::makeEditFailure(ed::EEditError::PRECONDITION_FAILED, 812));
                const auto &expected = context.direction == ed::EDirection::FORWARD ? before : after;
                if (*owner.value_ != expected)
                    return lux::cxx::unexpected(ed::makeEditFailure(ed::EEditError::PRECONDITION_FAILED, 813));
                auto reserved = budget.reserve(65536);
                if (!reserved) return lux::cxx::unexpected(reserved.error());
                try
                {
                    return ed::PreparedEditPtr{std::make_unique<Plan>(owner,
                        context.direction == ed::EDirection::FORWARD ? after : before)};
                }
                catch (const std::bad_alloc &)
                {
                    return lux::cxx::unexpected(ed::makeEditFailure(ed::EEditError::ALLOCATION_FAILURE));
                }
            }
        };
    public:
        std::unique_ptr<ed::EditHistory> history;
        bool reject{};
        unsigned publishes{}, notices{};
        FixtureSession()
        {
            auto made = ed::EditHistory::create({{128, 16777216, 1048576, 128}, {this,
                [](void *source, const ed::HistoryNotice &notice) noexcept {
                    auto &self = *static_cast<FixtureSession *>(source);
                    assert(self.history->view()->snapshot.current == notice.snapshot.current);
                    ++self.notices;
                }}, true});
            assert(made);
            history = std::move(*made);
        }
        ~FixtureSession() { assert(history->close()); }
        const inspector_fixture::Component &value() const noexcept { return *value_; }
        ed::EditOperationPtr operation(const inspector_fixture::Component &next)
        {
            return std::make_unique<Operation>(*this, next);
        }
        void commit(const inspector_fixture::Component &next)
        {
            auto input = operation(next);
            const auto result = history->execute(input);
            assert(result && !input);
        }
    };
}
