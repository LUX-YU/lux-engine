#pragma once
#include "TestChecks.hpp"
namespace lux::editor::editing::test
{
    inline void failureCases()
    {
        check(
            "F01",
            []
            {
                TextSession s;
                for (const auto offset : {std::size_t{6U}, (std::numeric_limits<std::size_t>::max)()})
                {
                    auto p = s.replace(offset, "a", "b");
                    rejected(s, p, EEditError::PRECONDITION_FAILED);
                }
                auto p = s.replace(0U, "alphax", "");
                rejected(s, p, EEditError::PRECONDITION_FAILED);
            }
        );
        check(
            "F02",
            []
            {
                for (std::size_t point = 1; point <= 2U; ++point)
                {
                    RecordSession s;
                    execute(s, s.patch(1, {}, 2));
                    assert(s.select(1));
                    auto p = s.patch(1, 2, {});
                    s.allocation_index = 0U;
                    s.fail_allocation = point;
                    rejected(s, p, EEditError::ALLOCATION_FAILURE);
                    assert(s.allocation_index == point && s.stats.plans == s.stats.plans_destroyed);
                    s.fail_allocation = 0U;
                    assert(s.history->execute(p) && !p && s.records().empty());
                }
            }
        );
#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
        check(
            "F03",
            []
            {
                TextSession s;
                branch(s);
                auto p = s.replace(0U, "B", "D");
                static_cast<Operation*>(p.get())->title.assign(128U, 'L');
                allocation_call = 0U;
                fail_call = 1U;
                detail::editDiagnostics().allocation = allocationProbe;
                rejected(s, p, EEditError::ALLOCATION_FAILURE);
                assert(allocation_call == 1U);
                detail::editDiagnostics() = {};
                assert(s.history->execute(p));
            }
        );
#endif
        check(
            "F04",
            []
            {
                auto limits = kLimits;
                limits.max_staging_bytes = 1U;
                TextSession s("alpha", limits);
                auto p = s.replace(0U, "alpha", "beta");
                rejected(s, p, EEditError::STAGING_LIMIT);
                assert(s.stats.allocations == 0U);
            }
        );
        check(
            "F05",
            []
            {
                class BudgetOperation final : public Operation
                {
                public:
                    using Operation::Operation;
                    EditResult<PreparedEditPtr> prepare(const ApplyContext&, EditPreparationBudget& budget)
                        const noexcept override
                    {
                        assert(budget.reserve(0U) && budget.used() == 0U);
                        assert(budget.reserve(budget.limit()) && budget.remaining() == 0U && budget.reserve(0U));
                        expectError(budget.reserve(1U), EEditError::STAGING_LIMIT);
                        expectError(
                            budget.reserve((std::numeric_limits<std::size_t>::max)()), EEditError::STAGING_LIMIT
                        );
                        assert(budget.used() == budget.limit());
                        return Session::error(EEditError::STAGING_LIMIT);
                    }
                };
                TextSession s;
                EditOperationPtr p = std::make_unique<BudgetOperation>(s);
                rejected(s, p, EEditError::STAGING_LIMIT);
            }
        );
        check(
            "F06",
            []
            {
                TextSession s;
                s.empty_plan = true;
                auto p = s.replace(0U, "alpha", "beta");
                rejected(s, p, EEditError::CONTRACT_VIOLATION);
            }
        );
        check(
            "F07",
            []
            {
                TextSession s;
                change(s, "B");
                s.fail_allocation = s.allocation_index + 1U;
                replayFailure(s, [&] { return s.history->undo(); }, EEditError::ALLOCATION_FAILURE);
                s.fail_allocation = 0U;
                assert(s.history->undo() && s.text() == "alpha");
            }
        );
        check(
            "F08",
            []
            {
                TextSession s;
                branch(s);
                s.fail_allocation = s.allocation_index + 2U;
                replayFailure(s, [&] { return s.history->redo(); }, EEditError::ALLOCATION_FAILURE);
                s.fail_allocation = 0U;
                assert(s.history->redo() && s.text() == "C");
            }
        );
        check(
            "F09",
            []
            {
                RecordSession s;
                execute(s, s.patch(1, {}, 2));
                std::size_t allocations{};
                s.callback = [&](EStage stage)
                {
                    if (stage == EStage::APPLY)
                    {
                        allocations = s.stats.allocations;
#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
                        detail::editDiagnostics().allocation = allocationProbe;
                        deny_allocation = true;
#endif
                    }
                    if (stage == EStage::NOTICE)
                    {
                        assert(s.stats.allocations == allocations);
                    }
                };
                execute(s, s.patch(1, 2, {}));
#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
                deny_allocation = false;
                detail::editDiagnostics() = {};
#endif
                s.callback = {};
            }
        );
        check(
            "F10",
            []
            {
                auto limits = kLimits;
                limits.max_entries = 2U;
                TextSession s("alpha", limits);
                change(s, "A");
                change(s, "B");
                auto p = s.replace(0U, "B", "C");
                static_cast<Operation*>(p.get())->title = "SSO";
                execute(s, std::move(p));
                assert(std::string_view(s.published_label.data()) == "SSO");
            }
        );
        check(
            "F11",
            []
            {
                for (int action = 0; action < 7; ++action)
                {
                    TextSession s;
                    auto nested = s.replace(0U, "alpha", "nested");
                    s.callback = [&](EStage stage)
                    {
                        if (stage != EStage::PREPARE)
                        {
                            return;
                        }
                        switch (action)
                        {
                        case 0:
                            expectError(s.history->execute(nested), EEditError::BUSY);
                            break;
                        case 1:
                            expectError(s.history->undo(), EEditError::BUSY);
                            break;
                        case 2:
                            expectError(s.history->redo(), EEditError::BUSY);
                            break;
                        case 3:
                            expectError(s.history->clear(), EEditError::BUSY);
                            break;
                        case 4:
                            expectError(s.history->beginSave(), EEditError::BUSY);
                            break;
                        case 5:
                            expectError(s.history->close(), EEditError::BUSY);
                            break;
                        case 6:
                            expectError(s.select(), EEditError::BUSY);
                            break;
                        }
                    };
                    change(s, "B");
                    assert(nested && s.stats.prepares == 1U);
                    s.callback = {};
                }
            }
        );
        check(
            "F12",
            []
            {
                TextSession s;
                s.observer = [&](const HistoryNotice& n)
                {
                    if (n.kind == EHistoryEvent::CLOSED)
                    {
                        return;
                    }
                    assert(sameSnapshot(n.snapshot, s.history->view()->snapshot));
                    assert(s.text() == (n.kind == EHistoryEvent::UNDONE ? "alpha" : "B"));
                    expectError(s.history->undo(), EEditError::BUSY);
                    expectError(s.history->beginSave(), EEditError::BUSY);
                };
                change(s, "B");
                assert(s.history->undo() && s.history->redo());
                s.observer = {};
            }
        );
        check(
            "F13",
            []
            {
                auto limits = kLimits;
                limits.max_entries = 1U;
                TextSession s("alpha", limits);
                s.callback = [&](EStage stage)
                {
                    if (stage == EStage::PLAN_DESTROY || stage == EStage::OPERATION_DESTROY)
                    {
                        const auto closed = s.history->view()->snapshot.closed;
                        expectError(s.history->undo(), closed ? EEditError::CLOSED : EEditError::BUSY);
                    }
                };
                change(s, "B");
                change(s, "C");
                assert(s.history->clear());
                change(s, "D");
                assert(s.history->close());
                s.callback = {};
                assert(s.stats.operations == s.stats.operations_destroyed && s.stats.plans == s.stats.plans_destroyed);
            }
        );
        check(
            "F14",
            []
            {
                TextSession a, b;
                auto p = b.replace(0U, "alpha", "B");
                a.callback = [&](EStage stage)
                {
                    if (stage == EStage::PUBLISH)
                    {
                        assert(b.history->execute(p));
                    }
                };
                change(a, "A");
                assert(a.text() == "A" && b.text() == "B");
                a.callback = {};
            }
        );
        check(
            "F15",
            []
            {
                TextSession s;
                auto p = s.replace(40U, "x", "y");
                rejected(s, p, EEditError::PRECONDITION_FAILED);
                change(s, "B");
                assert(s.text() == "B");
            }
        );
    }
} // namespace lux::editor::editing::test
