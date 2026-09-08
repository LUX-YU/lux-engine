#pragma once
#include "TestChecks.hpp"
namespace lux::editor::editing::test
{
    inline void coreCases()
    {
        check(
            "C01",
            []
            {
                TextSession s;
                const auto v = s.history->view()->snapshot;
                assert(v.history.valid() && v.current.serial == 1U && v.revision.value == 0U);
                assert(
                    v.event_sequence == 0U && v.entry_count == 0U && v.cursor == 0U && !v.clean && s.stats.notices == 0U
                );
            }
        );
        check(
            "C02",
            []
            {
                TextSession a;
                RecordSession b;
                const auto first = a.historyId();
                assert(first != b.historyId());
                assert(a.history->close());
                TextSession c;
                assert(c.historyId() != first && c.historyId() != b.historyId());
            }
        );
        check(
            "C03",
            []
            {
                for (std::size_t i = 0; i != 4; ++i)
                {
                    auto limits = kLimits;
                    std::size_t* slots[] = {
                        &limits.max_entries,
                        &limits.max_retained_bytes,
                        &limits.max_staging_bytes,
                        &limits.max_label_bytes
                    };
                    *slots[i] = 0U;
                    expectError(EditHistory::create({limits}), EEditError::INVALID_LIMITS);
                }
                auto limits = kLimits;
                limits.max_entries = (std::numeric_limits<std::size_t>::max)();
                expectError(EditHistory::create({limits}), EEditError::INVALID_LIMITS);
                expectError(EditHistory::create({kLimits, {&limits, nullptr}}), EEditError::INVALID_ARGUMENT);
                const auto f = makeEditFailure(EEditError::BUSY, 42U, std::string(400U, 'x'));
                assert(f.message[191] == '\0' && f.message_truncated && f.domain_code == 42U);
                const auto e = makeEditFailure(EEditError::BUSY, 0U, std::string_view("a\0b", 3U));
                assert(e.message[0] == 'a' && e.message[1] == '\0' && e.message_truncated);
                const auto terminal = makeEditFailure(EEditError::BUSY, 0U, std::string_view("a\0", 2U));
                assert(!terminal.message_truncated && terminal.message[1] == '\0');
            }
        );
#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
        check(
            "C04",
            []
            {
                detail::editDiagnostics().allocation = allocationProbe;
                for (fail_call = 1U; fail_call <= 4U; ++fail_call)
                {
                    const auto before = detail::allocationStatistics();
                    allocation_call = 0U;
                    expectError(EditHistory::create({kLimits}), EEditError::ALLOCATION_FAILURE);
                    assert(allocation_call == fail_call);
                    assert(detail::allocationStatistics() == before);
                    std::cout << "C04 allocation point " << fail_call << " retained no backing storage or objects\n";
                }
                detail::editDiagnostics() = {};
                assert(EditHistory::create({kLimits}));
            }
        );
#endif
        check(
            "C05",
            []
            {
                TextSession s;
                auto p = s.replace(0U, "alpha", "beta");
                const Snapshot before(s);
                const auto metadata = s.stats.metadata;
                std::thread foreign(
                    [&]
                    {
                        expectError(s.history->view(), EEditError::WRONG_THREAD);
                        expectError(s.history->entry(0U), EEditError::WRONG_THREAD);
                        expectError(s.history->execute(p), EEditError::WRONG_THREAD);
                        expectError(s.history->undo(), EEditError::WRONG_THREAD);
                        expectError(s.history->redo(), EEditError::WRONG_THREAD);
                        expectError(s.history->clear(), EEditError::WRONG_THREAD);
                        expectError(s.history->beginSave(), EEditError::WRONG_THREAD);
                        expectError(s.history->finishSave({}, ESaveOutcome::SUCCEEDED), EEditError::WRONG_THREAD);
                        expectError(s.history->close(), EEditError::WRONG_THREAD);
                    }
                );
                foreign.join();
                assert(p && before == Snapshot(s) && metadata == s.stats.metadata);
            }
        );
        check(
            "C06",
            []
            {
                TextSession s;
                EditOperationPtr p;
                rejected(s, p, EEditError::INVALID_ARGUMENT);
            }
        );
        check(
            "C07",
            []
            {
                TextSession a, b;
                auto p = a.replace(0U, "alpha", "beta");
                const Snapshot before(a);
                rejected(b, p, EEditError::WRONG_HISTORY);
                assert(before == Snapshot(a) && a.text() == "alpha");
            }
        );
        check(
            "C08",
            []
            {
                TextSession s;
                auto p = s.replace(0U, "alpha", "beta");
                branch(s);
                rejected(s, p, EEditError::STALE_BASE);
            }
        );
        check(
            "C09",
            []
            {
                TextSession s;
                for (const auto title : {std::string{}, std::string(257U, 'x'), std::string("a\0b", 3U)})
                {
                    auto p = s.replace(0U, "alpha", "beta");
                    static_cast<Operation*>(p.get())->title = title;
                    rejected(s, p, title.size() > 256U ? EEditError::HISTORY_LIMIT : EEditError::INVALID_ARGUMENT);
                }
                auto p = s.replace(0U, "alpha", "beta");
                static_cast<Operation*>(p.get())->charge = kLimits.max_retained_bytes + 1U;
                rejected(s, p, EEditError::HISTORY_LIMIT);
                assert(s.stats.prepares == 0U);
            }
        );
        check(
            "C10",
            []
            {
                TextSession s;
                change(s, "beta");
                const auto state = s.history->view()->snapshot.current;
                assert(s.text() == "beta" && s.history->view()->snapshot.cursor == 1U);
                assert(s.history->undo() && s.text() == "alpha" && s.history->view()->snapshot.cursor == 0U);
                assert(s.history->redo());
                const auto v = s.history->view()->snapshot;
                assert(s.text() == "beta" && v.current == state && v.revision.value == 3U && v.cursor == 1U);
            }
        );
        check(
            "C11",
            []
            {
                RecordSession s;
                execute(s, s.patch(1, {}, 12));
                assert(s.select(1));
                std::size_t business{}, notices{};
                const auto observe = [&]
                {
                    assert(!s.selected() || s.records().contains(*s.selected()));
                    assert(!s.history->view()->can_undo);
                };
                s.callback = [&](EStage stage)
                {
                    if (stage == EStage::PUBLISH)
                    {
                        observe();
                        ++business;
                    }
                };
                s.observer = [&](const HistoryNotice& n)
                {
                    if (n.kind != EHistoryEvent::CLOSED)
                    {
                        observe();
                        ++notices;
                    }
                };
                execute(s, s.patch(1, 12, {}));
                assert(s.records().empty() && !s.selected());
                assert(s.history->undo() && s.records().at(1) == 12 && s.selected() == 1);
                assert(s.history->redo() && s.records().empty() && !s.selected() && business == 3U && notices == 3U);
                s.callback = {};
                s.observer = {};
            }
        );
        check(
            "C12",
            []
            {
                TextSession s;
                expectError(s.history->undo(), EEditError::NO_UNDO);
                expectError(s.history->redo(), EEditError::NO_REDO);
                assert(s.stats.prepares == 0U && s.stats.notices == 0U);
            }
        );
        check(
            "C13",
            []
            {
                RecordSession s;
                execute(s, s.patch(1, {}, 10));
                execute(s, s.patch(2, {}, 20));
                assert(s.history->undo() && !s.records().contains(2) && s.records().at(1) == 10);
                assert(s.history->undo() && s.records().empty());
                assert(s.history->redo() && s.records().at(1) == 10);
                assert(s.history->redo() && s.records().at(2) == 20);
            }
        );
        check(
            "C14",
            []
            {
                TextSession s;
                change(s, "A");
                change(s, "B");
                change(s, "C");
                const auto old = s.history->view()->snapshot.current;
                assert(s.history->undo() && s.history->undo());
                change(s, "D");
                assert(s.stats.operations_destroyed == 2U && s.history->view()->snapshot.entry_count == 2U);
                assert(s.history->view()->snapshot.current.serial > old.serial);
                assert(s.history->undo() && s.text() == "A");
            }
        );
        check(
            "C15",
            []
            {
                TextSession s;
                branch(s);
                s.reject_prepare = true;
                auto p = s.replace(0U, "B", "D");
                rejected(s, p, EEditError::PRECONDITION_FAILED);
            }
        );
        check(
            "C16",
            []
            {
                TextSession s;
                branch(s);
                const Snapshot before(s);
                const auto publishes = s.stats.publishes;
                auto p = s.replace(0U, "B", "B");
                const auto result = s.history->execute(p);
                assert(result && result->effect == EEditEffect::NO_CHANGE && !p);
                assert(before == Snapshot(s) && publishes == s.stats.publishes);
            }
        );
        check(
            "C17",
            []
            {
                TextSession s;
                auto p = s.replace(100U, "", "");
                rejected(s, p, EEditError::PRECONDITION_FAILED);
            }
        );
        check(
            "C18",
            []
            {
                TextSession s;
                change(s, "B");
                s.force_no_change = true;
                replayFailure(s, [&] { return s.history->undo(); }, EEditError::CONTRACT_VIOLATION);
                s.force_no_change = false;
                assert(s.history->undo());
                s.force_no_change = true;
                replayFailure(s, [&] { return s.history->redo(); }, EEditError::CONTRACT_VIOLATION);
            }
        );
        check(
            "C19",
            []
            {
                TextSession s;
                std::uint64_t previous = 1U;
                for (int i = 0; i < 12; ++i)
                {
                    change(s, std::to_string(i));
                    const auto current = s.history->view()->snapshot.current.serial;
                    assert(current > previous);
                    previous = current;
                    assert(s.history->undo());
                }
                assert(s.history->view()->snapshot.revision.value == 24U);
            }
        );
        check(
            "C20",
            []
            {
                TextSession s;
                auto p = s.replace(0U, "alpha", "beta");
                assert(s.history->execute(p) && !p && s.stats.operations_destroyed == 0U);
                assert(s.history->undo() && s.stats.operations_destroyed == 0U);
                assert(s.history->close() && s.stats.operations_destroyed == 1U);
            }
        );
        check(
            "C21",
            []
            {
                TextSession s;
                auto p = s.replace(0U, "alpha", "beta");
                s.reject_prepare = true;
                rejected(s, p, EEditError::PRECONDITION_FAILED);
                s.reject_prepare = false;
                assert(s.history->execute(p) && !p && s.text() == "beta");
            }
        );
        check(
            "C22",
            []
            {
                TextSession s;
                s.callback = [&](EStage stage)
                {
                    if (stage == EStage::OPERATION_DESTROY || stage == EStage::PLAN_DESTROY)
                    {
                        assert(s.history->view()->phase == EHistoryPhase::RECLAIMING);
                        expectError(s.history->clear(), EEditError::BUSY);
                    }
                };
                execute(s, s.replace(0U, "alpha", "alpha"));
                assert(s.stats.operations_destroyed == 1U && s.stats.plans_destroyed == 1U);
                s.callback = {};
            }
        );
    }
} // namespace lux::editor::editing::test
