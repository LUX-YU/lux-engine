#pragma once
#include "EditingFixtures.hpp"
#include <iostream>
#include <lux/engine/editor/editing/detail/EditDiagnostics.hpp>
#include <thread>

namespace lux::editor::editing::test
{
    template <class F> void check(const char* id, F body)
    {
        std::cout << id << " BEGIN" << std::endl;
        body();
        std::cout << id << " PASS" << std::endl;
    }
    template <class T> void expectError(const EditResult<T>& result, EEditError expected)
    {
        assert(!result && result.error().code == expected);
    }
    inline auto model(const TextSession& value)
    {
        return value.text();
    }
    inline auto model(const RecordSession& value)
    {
        return std::pair(value.records(), value.selected());
    }
    template <class S> void rejected(S& session, EditOperationPtr& operation, EEditError code)
    {
        const Snapshot before(session);
        const auto content = model(session);
        const auto notices = session.stats.notices;
        auto* pointer = operation.get();
        auto* data = dynamic_cast<Operation*>(pointer);
        const auto identity = data ? data->identity : HistoryId{};
        const auto base = data ? data->base : StateId{};
        const auto title = data ? data->title : std::string{};
        const auto charge = data ? data->charge : 0U;
        const auto memento = data ? data->memento() : std::string{};
        expectError(session.history->execute(operation), code);
        assert(operation.get() == pointer && before == Snapshot(session));
        assert(model(session) == content && session.stats.notices == notices);
        if (data)
        {
            assert(data->memento() == memento);
            assert(data->identity == identity && data->base == base && data->title == title && data->charge == charge);
        }
    }
    inline void change(TextSession& session, std::string next)
    {
        execute(session, session.replace(0U, session.text(), std::move(next)));
    }
    inline void branch(TextSession& session)
    {
        change(session, "B");
        change(session, "C");
        assert(session.history->undo());
    }
    template <class F> void replayFailure(TextSession& session, F replay, EEditError code)
    {
        const Snapshot before(session);
        const auto text = session.text();
        expectError(replay(), code);
        assert(before == Snapshot(session) && text == session.text());
    }
#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
    inline std::size_t allocation_call{}, fail_call{};
    inline bool deny_allocation{};
    inline void allocationProbe(detail::EEditAllocationSite, std::size_t)
    {
        assert(!deny_allocation);
        if (++allocation_call == fail_call)
        {
            throw std::bad_alloc{};
        }
    }
#endif
} // namespace lux::editor::editing::test
