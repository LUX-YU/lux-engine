#pragma once
#include "TestChecks.hpp"
namespace lux::editor::editing::test
{
    inline void saveLimitCases()
    {
        check(
            "L01",
            []
            {
                auto limits = kLimits;
                limits.max_entries = 2U;
                TextSession s("alpha", limits);
                change(s, "A");
                change(s, "B");
                change(s, "C");
                assert(s.history->undo() && s.history->undo() && s.text() == "A");
                expectError(s.history->undo(), EEditError::NO_UNDO);
            }
        );
        check(
            "L02",
            []
            {
                auto limits = kLimits;
                limits.max_retained_bytes = 240U;
                TextSession s("alpha", limits);
                for (int i = 0; i < 3; ++i)
                {
                    auto p = s.replace(0U, s.text(), std::to_string(i));
                    static_cast<Operation*>(p.get())->charge = 100U;
                    execute(s, std::move(p));
                }
                const auto v = s.history->view()->snapshot;
                assert(v.entry_count == 2U && v.charged_retained_bytes <= 240U && s.stats.operations_destroyed == 1U);
            }
        );
        check(
            "L03",
            []
            {
                auto limits = kLimits;
                limits.max_retained_bytes = 240U;
                TextSession s("alpha", limits);
                for (const auto value : {"A", "B"})
                {
                    auto p = s.replace(0U, s.text(), value);
                    static_cast<Operation*>(p.get())->charge = 100U;
                    execute(s, std::move(p));
                }
                assert(s.history->undo());
                auto p = s.replace(0U, "A", "C");
                static_cast<Operation*>(p.get())->charge = 100U;
                execute(s, std::move(p));
                assert(s.history->view()->snapshot.entry_count == 2U);
            }
        );
        check(
            "L04",
            []
            {
                TextSession s;
                auto p = s.replace(0U, "alpha", "B");
                static_cast<Operation*>(p.get())->charge = (std::numeric_limits<std::size_t>::max)();
                rejected(s, p, EEditError::HISTORY_LIMIT);
                assert(s.stats.prepares == 0U);
            }
        );
        check(
            "L05",
            []
            {
                struct SharedOperation final : Operation
                {
                    EditOperationPtr delegate;
                    std::shared_ptr<const std::string> shared;
                    SharedOperation(TextSession& s, std::shared_ptr<const std::string> data)
                        : Operation(s), delegate(s.replace(0U, s.text(), *data)), shared(std::move(data))
                    {
                        charge = sizeof(SharedOperation) + 1024U;
                    }
                    EditResult<PreparedEditPtr> prepare(const ApplyContext& c, EditPreparationBudget& b)
                        const noexcept override
                    {
                        return delegate->prepare(c, b);
                    }
                };
                TextSession a, b;
                auto data = std::make_shared<const std::string>("shared");
                std::weak_ptr weak = data;
                execute(a, std::make_unique<SharedOperation>(a, data));
                execute(b, std::make_unique<SharedOperation>(b, data));
                data.reset();
                assert(a.history->clear() && !weak.expired());
                assert(b.history->clear() && weak.expired());
            }
        );
        check(
            "L06",
            []
            {
                TextSession s;
                branch(s);
                const auto ticket = *s.history->beginSave();
                const auto before = s.history->view()->snapshot;
                assert(s.history->clear());
                const auto after = s.history->view()->snapshot;
                assert(
                    after.current == before.current && after.saved == before.saved && after.revision == before.revision
                );
                assert(
                    after.save_pending && after.entry_count == 0U && after.event_sequence == before.event_sequence + 1U
                );
                assert(s.history->finishSave(ticket, ESaveOutcome::SUCCEEDED));
            }
        );
        check(
            "L07",
            []
            {
                TextSession s;
                const Snapshot before(s);
                assert(s.history->clear() && s.history->clear());
                assert(before == Snapshot(s));
            }
        );
#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
        check(
            "L08",
            []
            {
                constexpr auto max = (std::numeric_limits<std::uint64_t>::max)();
                for (int slot = 0; slot < 4; ++slot)
                {
                    TextSession s;
                    auto p = s.replace(0U, "alpha", "B");
                    detail::EditHistoryTestAccess::counters(
                        *s.history,
                        slot == 0 ? max : 1U,
                        slot == 1 ? max : 0U,
                        slot == 2 ? max : 0U,
                        slot == 3 ? max : 0U
                    );
                    if (slot == 3)
                    {
                        expectError(s.history->beginSave(), EEditError::ID_EXHAUSTED);
                    }
                    else
                    {
                        rejected(s, p, EEditError::ID_EXHAUSTED);
                    }
                }
            }
        );
        check(
            "L09",
            []
            {
                TextSession s;
                change(s, "B");
                const auto max = (std::numeric_limits<std::uint64_t>::max)();
                detail::EditHistoryTestAccess::counters(*s.history, 2U, 1U, max, 0U);
                assert(s.history->close());
                const auto v = s.history->view()->snapshot;
                assert(v.closed && v.event_sequence == max && s.stats.operations_destroyed == 1U);
                const auto notices = s.stats.notices;
                assert(s.history->close() && notices == s.stats.notices);
            }
        );
#endif
        check(
            "S01",
            []
            {
                TextSession s("alpha", kLimits, true);
                assert(s.history->view()->snapshot.clean);
                change(s, "B");
                assert(!s.history->view()->snapshot.clean);
                assert(s.history->undo() && s.history->view()->snapshot.clean);
                assert(s.history->redo() && !s.history->view()->snapshot.clean);
            }
        );
        check(
            "S02",
            []
            {
                TextSession s;
                change(s, "B");
                const auto ticket = *s.history->beginSave();
                change(s, "C");
                assert(s.history->finishSave(ticket, ESaveOutcome::SUCCEEDED));
                assert(s.history->view()->snapshot.saved == ticket.state() && !s.history->view()->snapshot.clean);
                assert(s.history->undo() && s.history->view()->snapshot.clean);
            }
        );
        check(
            "S03",
            []
            {
                TextSession s;
                const auto ticket = *s.history->beginSave();
                const Snapshot before(s);
                expectError(s.history->beginSave(), EEditError::SAVE_IN_PROGRESS);
                assert(before == Snapshot(s));
                assert(s.history->finishSave(ticket, ESaveOutcome::CANCELLED));
            }
        );
        check(
            "S04",
            []
            {
                TextSession a, b;
                const auto ticket = *a.history->beginSave();
                const Snapshot before(b);
                expectError(b.history->finishSave(ticket, ESaveOutcome::SUCCEEDED), EEditError::STALE_SAVE);
                expectError(b.history->finishSave({}, ESaveOutcome::SUCCEEDED), EEditError::STALE_SAVE);
                assert(before == Snapshot(b));
                assert(a.history->close());
                expectError(a.history->finishSave(ticket, ESaveOutcome::SUCCEEDED), EEditError::CLOSED);
            }
        );
        check(
            "S05",
            []
            {
                TextSession s;
                const auto ticket = *s.history->beginSave();
                assert(s.history->finishSave(ticket, ESaveOutcome::SUCCEEDED));
                const Snapshot before(s);
                expectError(s.history->finishSave(ticket, ESaveOutcome::SUCCEEDED), EEditError::STALE_SAVE);
                assert(before == Snapshot(s));
            }
        );
        check(
            "S06",
            []
            {
                for (const auto outcome : {ESaveOutcome::FAILED, ESaveOutcome::CANCELLED})
                {
                    TextSession s("alpha", kLimits, true);
                    const auto saved = s.history->view()->snapshot.saved;
                    change(s, "B");
                    auto ticket = *s.history->beginSave();
                    assert(s.history->finishSave(ticket, outcome));
                    assert(s.history->view()->snapshot.saved == saved && !s.history->view()->snapshot.save_pending);
                    ticket = *s.history->beginSave();
                    assert(s.history->finishSave(ticket, ESaveOutcome::SUCCEEDED));
                }
            }
        );
        check(
            "S07",
            []
            {
                TextSession s;
                const auto ticket = *s.history->beginSave();
                s.callback = [&](EStage stage)
                {
                    if (stage == EStage::PUBLISH)
                    {
                        expectError(s.history->finishSave(ticket, ESaveOutcome::SUCCEEDED), EEditError::BUSY);
                    }
                };
                change(s, "B");
                s.callback = {};
                assert(s.history->finishSave(ticket, ESaveOutcome::SUCCEEDED));
                assert(s.history->view()->snapshot.saved == ticket.state() && !s.history->view()->snapshot.clean);
            }
        );
        check(
            "S08",
            []
            {
                TextSession s;
                change(s, "B");
                const auto ticket = *s.history->beginSave();
                assert(s.history->undo());
                change(s, "C");
                assert(s.history->finishSave(ticket, ESaveOutcome::SUCCEEDED));
                assert(s.history->view()->snapshot.saved == ticket.state() && !s.history->view()->snapshot.clean);
            }
        );
        check(
            "S09",
            []
            {
                TextSession old;
                const auto ticket = *old.history->beginSave();
                assert(old.history->close());
                TextSession fresh;
                expectError(old.history->finishSave(ticket, ESaveOutcome::SUCCEEDED), EEditError::CLOSED);
                expectError(fresh.history->finishSave(ticket, ESaveOutcome::SUCCEEDED), EEditError::STALE_SAVE);
                assert(fresh.stats.prepares == 0U);
            }
        );
    }
} // namespace lux::editor::editing::test
