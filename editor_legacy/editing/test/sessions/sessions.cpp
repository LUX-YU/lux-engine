#include "ObjectQueue.hpp"
#include <lux/engine/editor/editing/EditExecutor.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

using namespace lux::editor;
using namespace lux::editor::sessions;

namespace
{
    struct Model final
    {
        int value{};
        bool reject{};
        bool no_change{};
        std::vector<int>* destruction_order{};
        ~Model() noexcept
        {
            if (destruction_order)
                destruction_order->push_back(2);
        }
    };
    class Prepared final : public editing::PreparedEdit
    {
    public:
        Prepared(Model& model, int value, editing::EEditEffect effect) : model_(model), value_(value), effect_(effect)
        {}
        editing::EEditEffect effect() const noexcept override
        {
            return effect_;
        }

    private:
        void apply() noexcept override
        {
            model_.value = value_;
        }
        void publish(const editing::CommitInfo&) noexcept override {}
        Model& model_;
        int value_;
        editing::EEditEffect effect_;
    };
    class Operation final : public editing::EditOperation
    {
    public:
        Operation(editing::StateId base, Model& model, int next)
            : base_(base), model_(model), before_(model.value), next_(next)
        {}
        ~Operation() noexcept override
        {
            if (model_.destruction_order)
                model_.destruction_order->push_back(1);
        }
        editing::HistoryId historyId() const noexcept override
        {
            return base_.history;
        }
        editing::StateId baseState() const noexcept override
        {
            return base_;
        }
        std::string_view label() const noexcept override
        {
            return "set";
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            return 64;
        }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext& context,
            editing::EditPreparationBudget& budget
        ) const noexcept override
        {
            if (model_.reject)
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
            auto reserved = budget.reserve(32);
            if (!reserved)
                return lux::cxx::unexpected(reserved.error());
            return std::make_unique<Prepared>(
                model_,
                context.direction == editing::EDirection::FORWARD ? next_ : before_,
                model_.no_change ? editing::EEditEffect::NO_CHANGE : editing::EEditEffect::CHANGE
            );
        }

    private:
        editing::StateId base_;
        Model& model_;
        int before_;
        int next_;
    };
    std::unique_ptr<editing::EditHistory> history(std::size_t staging = 1024)
    {
        auto result = editing::EditHistory::create({{8, 4096, staging, 64}, {}});
        assert(result);
        return std::move(*result);
    }
    editing::EditResult<editing::ApplyResult> edit(editing::EditHistory& history, Model& model, int value)
    {
        editing::EditOperationPtr operation =
            std::make_unique<Operation>(history.view()->snapshot.current, model, value);
        auto* before = operation.get();
        auto result = editing::EditExecutor{}.execute(history, operation);
        if (!result)
            assert(operation.get() == before);
        return result;
    }
    class FakeSession final : public IEditSession
    {
    public:
        FakeSession(SessionId id, std::vector<int>* order = nullptr, SourceBinding binding = {})
            : state_(id, std::move(binding)), history_(history())
        {
            source_.destruction_order = order;
            if (order)
                assert(edit(*history_, source_, 1));
        }
        ~FakeSession() noexcept override
        {
            if (description_count_)
                *description_count_ = describe_calls_;
        }
        void observeDescriptionCount(std::size_t& count) noexcept
        {
            description_count_ = &count;
        }
        SessionInfo describe() const override
        {
            ++describe_calls_;
            if (reject_describe_)
                throw std::runtime_error("injected full description failure");
            return {
                state_.id(),
                {"test.fake"},
                state_.binding(),
                stamp(),
                state_.observed(),
                !state_.checkpoint().clean(stamp().state, state_.bindingRevision()),
                state_.admission()
            };
        }
        ContentStamp stamp() const noexcept
        {
            return {state_.id(), history_->view()->snapshot.current};
        }
        void rejectDescribe(bool reject) noexcept
        {
            reject_describe_ = reject;
        }
        std::size_t describeCalls() const noexcept
        {
            return describe_calls_;
        }
        void changeContent()
        {
            assert(edit(*history_, source_, source_.value + 1));
            state_.contentChanged();
        }
        SessionState& state()
        {
            return state_;
        }

    private:
        ContentStamp currentContent() const noexcept override
        {
            return stamp();
        }
        SessionResult<ClosePermit> prepareClose(ContentStamp expected) noexcept override
        {
            return state_.prepareClose(stamp(), expected);
        }
        SessionState state_;
        Model source_;
        std::unique_ptr<editing::EditHistory> history_;
        mutable std::size_t describe_calls_{};
        bool reject_describe_{};
        std::size_t* description_count_{};
    };
    class OtherSession final : public IEditSession
    {
        ContentStamp currentContent() const noexcept override
        {
            return {};
        }
        SessionInfo describe() const override
        {
            return {};
        }
        SessionResult<ClosePermit> prepareClose(ContentStamp) noexcept override
        {
            return lux::cxx::unexpected(ESessionError::INVALID_ARGUMENT);
        }
    };
    template <class T>
    concept HasSave = requires(T& value) { value.beginSave(); };
    template <class T>
    concept HasClean = requires(T value) {
        value.clean;
        value.saved;
    };
    static_assert(!HasSave<editing::EditHistory> && !HasClean<editing::HistorySnapshot>);
    static_assert(!std::is_copy_constructible_v<EditScope> && !std::is_move_constructible_v<EditScope>);
    static_assert(!std::is_copy_constructible_v<ClosePermit> && std::is_nothrow_move_constructible_v<ClosePermit>);

    void descriptionExceptionRecovery()
    {
        lux::test::ObjectQueue store_messages;
        SessionStore store{store_messages.dispatcherRef(), 2};
        auto reserved = store.reserve<FakeSession>({"test.fake"}, lux::object::CodeLease::builtin());
        auto candidate = std::make_unique<FakeSession>(reserved->id());
        auto* session = candidate.get();
        assert(store.prepare(*reserved, candidate) && store.publish(*reserved));
        session->rejectDescribe(true);
        bool caught{};
        try
        {
            (void)store.describe(reserved->id());
        }
        catch (const std::runtime_error&)
        {
            caught = true;
        }
        assert(caught);
        session->rejectDescribe(false);
        assert(store.describe(reserved->id()));
        auto other = store.reserve<FakeSession>({"test.fake"}, lux::object::CodeLease::builtin());
        assert(other); // Unwinding the public query must release CallbackScope.
        auto permit = store.prepareClose(session->stamp());
        assert(permit && store.close(*permit));
        std::puts("PASS X01-R1-04: describe exception restores callback admission");
    }
    bool closeContent(bool throws)
    {
        lux::test::ObjectQueue store_messages;
        SessionStore store{store_messages.dispatcherRef(), 1};
        auto reserved = store.reserve<FakeSession>({"test.fake"}, lux::object::CodeLease::builtin());
        BoundSource binding;
        binding.location.assign(32768, 'x');
        auto candidate = std::make_unique<FakeSession>(reserved->id(), nullptr, std::move(binding));
        auto* session = candidate.get();
        assert(store.prepare(*reserved, candidate) && store.publish(*reserved));
        const auto stamp = session->stamp();
        {
            auto stale = store.prepareClose(stamp);
            assert(stale);
            session->changeContent(); // Fault injection: violate the producer's gate to test commit validation.
            assert(!store.close(*stale) && store.size() == 1);
            assert(session->state().admission() == EEditAdmission::CLOSING);
        }
        assert(session->state().admission() == EEditAdmission::AVAILABLE);
        session->rejectDescribe(throws);
        auto permit = store.prepareClose(session->stamp());
        assert(permit);
        // Keep a scalar outside the object: close destroys the session.
        const auto calls = session->describeCalls();
        auto key = store.key<FakeSession>(reserved->id());
        assert(key);
        // The observer is only a test counter; it does not participate in identity or admission.
        std::size_t final_calls{};
        session->observeDescriptionCount(final_calls);
        assert(store.close(*permit) && store.size() == 0);
        assert(store_messages.collect() == 1);
        const bool used_description = final_calls != calls;
        std::printf(
            "%s X01-R1-0%d: close describe calls before=%zu after=%zu long_binding=32768\n",
            used_description ? "FAIL" : "PASS",
            throws ? 1 : 2,
            calls,
            final_calls
        );
        assert(!store.close(*permit) && !store.access<FakeSession>().read(*key));
        return !used_description;
    }
    void publishedCodeLifetime()
    {
        std::vector<int> order;
        lux::test::ObjectQueue store_messages;
        SessionStore store{store_messages.dispatcherRef(), 1};
        auto code = std::shared_ptr<const void>(new int{}, [&order](const void* p) {
            order.push_back(3);
            delete static_cast<const int*>(p);
        });
        auto reserved = store.reserve<FakeSession>({"test.fake"}, lux::object::CodeLease::plugin(code));
        code.reset();
        auto session = std::make_unique<FakeSession>(reserved->id(), &order);
        assert(store.prepare(*reserved, session) && store.publish(*reserved));
        auto permit = store.prepareClose(store.describe(reserved->id())->current);
        assert(permit && store.close(*permit));
        assert(store_messages.collect() == 1);
        assert((order == std::vector<int>{1, 2, 3}));
        std::puts("PASS X01-R1-05: published close destroys history, source, then code");
    }
    void checkpointAndHistory()
    {
        Model source;
        auto edits = history();
        PersistenceCheckpoint checkpoint;
        const auto baseline = edits->view()->snapshot.current;
        assert(!checkpoint.clean(baseline, {}));
        checkpoint.loaded(baseline, {});
        assert(edit(*edits, source, 1));
        assert(!checkpoint.clean(edits->view()->snapshot.current, {}));
        assert(editing::EditExecutor{}.undo(*edits) && source.value == 0);
        assert(checkpoint.clean(edits->view()->snapshot.current, {}));
        const auto undone = edits->view()->snapshot;
        source.no_change = true;
        assert(edit(*edits, source, 99)->effect == editing::EEditEffect::NO_CHANGE);
        const auto unchanged = edits->view()->snapshot;
        assert(unchanged.current == undone.current && unchanged.revision == undone.revision);
        assert(unchanged.event_sequence == undone.event_sequence && unchanged.cursor == undone.cursor);
        assert(unchanged.entry_count == undone.entry_count && edits->view()->can_redo && source.value == 0);
        source.no_change = false;
        assert(editing::EditExecutor{}.redo(*edits) && source.value == 1);
        assert(edit(*edits, source, 0)); // Same bytes, different history state, still dirty.
        assert(!checkpoint.clean(edits->view()->snapshot.current, {}));
        const auto current = edits->view()->snapshot.current;
        assert(checkpoint.accept(current, {}, {baseline, {}, {3}}));
        assert(!checkpoint.accept(current, {}, {current, {}, {2}}));
        assert(checkpoint.accept(current, {}, {current, {}, {4}}));
        assert(checkpoint.clean(current, {}));
        assert(editing::EditExecutor{}.clear(*edits) && checkpoint.clean(edits->view()->snapshot.current, {}));
        const auto replacement = history();
        assert(!checkpoint.accept(replacement->view()->snapshot.current, {}, {current, {}, {5}}));
        assert(!checkpoint.accept(current, {2}, {current, {1}, {5}}));
        source.reject = true;
        const auto before = edits->view()->snapshot;
        assert(!edit(*edits, source, 7));
        const auto after = edits->view()->snapshot;
        assert(source.value == 0 && after.current == before.current && after.cursor == before.cursor);
        source.reject = false;
        source.no_change = true;
        assert(edit(*edits, source, 0)->effect == editing::EEditEffect::NO_CHANGE);
        assert(edits->view()->snapshot.current == before.current);
        auto limited = history(1);
        assert(!edit(*limited, source, 4) && source.value == 0);
        assert(editing::EditExecutor{}.close(*edits));
    }
    void slotsAndPermits()
    {
        lux::test::ObjectQueue store_messages;
        SessionStore store{store_messages.dispatcherRef(), 1};
        auto reservation = store.reserve<FakeSession>({"test.fake"}, lux::object::CodeLease::builtin());
        assert(reservation && store.size() == 0);
        const auto first = reservation->id();
        assert(!store.describe(first));
        auto session = std::make_unique<FakeSession>(first);
        assert(store.prepare(*reservation, session) && !session && !store.describe(first));
        assert(store.publish(*reservation) && store.size() == 1);
        auto key = store.key<FakeSession>(first);
        assert(key && !store.key<OtherSession>(first));
        lux::test::ObjectQueue other_messages;
        SessionStore other{other_messages.dispatcherRef(), 1};
        assert(!other.key<FakeSession>(first));
        bool wrong_thread{};
        std::thread thread([&] { wrong_thread = store.describe(first).error() == ESessionError::WRONG_THREAD; });
        thread.join();
        assert(wrong_thread);
        auto access = store.access<FakeSession>();
        auto& state = access.edit(*key)->get().state();
        const auto stamp = access.read(*key)->get().stamp();
        auto edited = state.gate().withEdit([&](EditScope&) -> SessionResult<void> {
            assert(!state.gate().withEdit([](EditScope&) -> SessionResult<void> { return {}; }));
            assert(!store.prepareClose(stamp));
            return {};
        });
        assert(edited && state.admission() == EEditAdmission::AVAILABLE);
        {
            auto close = store.prepareClose(stamp);
            assert(close && store.describe(first));
            auto moved = std::move(*close);
            assert(!store.close(*close));
            assert(state.admission() == EEditAdmission::CLOSING);
        }
        assert(state.admission() == EEditAdmission::AVAILABLE);
        {
            SessionState impostor{first};
            auto forged = impostor.prepareClose(stamp, stamp);
            assert(forged && !store.close(*forged) && store.size() == 1);
        }
        {
            auto binding = state.prepareBindingChange(stamp, stamp);
            assert(binding);
            auto moved = std::move(*binding);
            assert(state.admission() == EEditAdmission::REBINDING);
        }
        assert(state.admission() == EEditAdmission::AVAILABLE);
        {
            auto binding = state.prepareBindingChange(stamp, stamp);
            assert(binding);
            BoundSource source;
            source.location = "/Project/Changed.asset";
            assert(state.rebind(*binding, source, stamp.state, {1}));
            assert(state.checkpoint().clean(stamp.state, state.bindingRevision()));
            assert(!state.accept(stamp, stamp, {1}, {2}));
            assert(!state.accept({{999, 0, 1}, stamp.state}, stamp, state.bindingRevision(), {2}));
        }
        auto close = store.prepareClose(stamp);
        assert(close && store.close(*close) && !access.read(*key));
        auto second = store.reserve<FakeSession>({"test.fake"}, lux::object::CodeLease::builtin());
        assert(second && second->id().slot == first.slot && second->id().generation != first.generation);
        session = std::make_unique<FakeSession>(second->id());
        assert(store.prepare(*second, session) && store.publish(*second));
        assert(!store.key<FakeSession>(first) && !store.close(*close));
        auto next = store.prepareClose(store.describe(second->id())->current);
        assert(next && store.close(*next));
    }
    void candidatesAndCode()
    {
        lux::test::ObjectQueue store_messages;
        SessionStore store{store_messages.dispatcherRef(), 1};
        assert(!store.reserve<FakeSession>({"test.fake"}, lux::object::CodeLease::plugin({})));
        std::vector<int> order;
        {
            auto code = std::shared_ptr<const void>(new int{}, [&order](const void* p) {
                order.push_back(3);
                delete static_cast<const int*>(p);
            });
            auto reservation = store.reserve<FakeSession>({"test.fake"}, lux::object::CodeLease::plugin(code));
            code.reset();
            assert(reservation && !store.publish(*reservation));
            assert(!store.reserve<FakeSession>({"test.fake"}, lux::object::CodeLease::builtin()));
            auto invalid = std::make_unique<FakeSession>(SessionId{999, 0, 1});
            assert(!store.prepare(*reservation, invalid) && invalid && store.size() == 0);
            auto candidate = std::make_unique<FakeSession>(reservation->id(), &order);
            assert(store.prepare(*reservation, candidate));
            assert(order.empty() && store.size() == 0);
        }
        assert(store_messages.collect() == 1);
        assert((order == std::vector<int>{1, 2, 3}) && store.size() == 0);
    }
    void closeSet()
    {
        lux::test::ObjectQueue store_messages;
        SessionStore store{store_messages.dispatcherRef(), 3};
        std::vector<FakeSession*> models;
        std::vector<SessionId> ids;
        for (int i{}; i < 2; ++i)
        {
            auto reserved = store.reserve<FakeSession>({"test.fake"}, lux::object::CodeLease::builtin());
            auto model = std::make_unique<FakeSession>(reserved->id());
            models.push_back(model.get());
            ids.push_back(reserved->id());
            assert(store.prepare(*reserved, model) && store.publish(*reserved));
        }
        const auto frozen_ids = store.snapshotIds();
        assert(frozen_ids && *frozen_ids == ids);
        auto hidden = store.reserve<FakeSession>({"test.fake"}, lux::object::CodeLease::builtin());
        assert(hidden && store.snapshotIds() == frozen_ids);
        std::vector<ClosePermit> permits;
        for (const auto* model : models)
            permits.push_back(std::move(*store.prepareClose(model->stamp())));
        models.back()->changeContent(); // Producer fault: all permits must be checked before A is removed.
        assert(store.close(permits).error() == ESessionError::STALE_CONTENT);
        assert(store.size() == 2 && store.snapshotIds() == frozen_ids);
        for (auto* model : models)
            assert(model->state().admission() == EEditAdmission::CLOSING);
        permits.clear();
        for (auto* model : models)
        {
            assert(model->state().admission() == EEditAdmission::AVAILABLE);
            model->rejectDescribe(true); // Final commit uses the private, allocation-free content stamp.
            permits.push_back(std::move(*store.prepareClose(model->stamp())));
        }
        assert(store.close(permits) && store.size() == 0 && store.snapshotIds()->empty());
        assert(!store.close(permits));
        assert(*frozen_ids == ids); // The owned observation survives reclamation.
        auto next = store.reserve<FakeSession>({"test.fake"}, lux::object::CodeLease::builtin());
        assert(next && next->id() != ids.front() && next->id() != ids.back());
        bool wrong_thread{};
        std::thread thread([&] { wrong_thread = store.snapshotIds().error() == ESessionError::WRONG_THREAD; });
        thread.join();
        assert(wrong_thread);
        std::puts("PASS P12 Store close set: no partial removal, hidden slots excluded, owned IDs, private stamps");
    }
}
#include "executor.hpp"

int main(int argc, char** argv)
{
    executorContracts();
    std::set_terminate([] {
        std::fputs("FAIL close crossed noexcept through complete describe; terminate\n", stderr);
        std::fflush(stderr);
        std::_Exit(86);
    });
    if (argc == 2 && std::string_view(argv[1]) == "close-throw")
        return closeContent(true) ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "close-contract")
        return closeContent(false) ? 0 : 1;
    checkpointAndHistory();
    slotsAndPermits();
    candidatesAndCode();
    descriptionExceptionRecovery();
    publishedCodeLifetime();
    closeSet();
    if (!closeContent(false))
        return 1;
    return closeContent(true) ? 0 : 1;
}
