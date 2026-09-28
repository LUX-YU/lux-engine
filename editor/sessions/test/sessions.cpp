#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <cassert>
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
        auto result = history.execute(operation);
        if (!result)
            assert(operation.get() == before);
        return result;
    }
    class FakeSession final : public IEditSession
    {
    public:
        FakeSession(SessionId id, std::vector<int>* order = nullptr) : state_(id), history_(history())
        {
            source_.destruction_order = order;
            if (order)
                assert(edit(*history_, source_, 1));
        }
        SessionInfo describe() const override
        {
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
        ContentStamp stamp() const
        {
            return {state_.id(), history_->view()->snapshot.current};
        }
        SessionState& state()
        {
            return state_;
        }

    private:
        SessionResult<ClosePermit> prepareClose(ContentStamp expected) noexcept override
        {
            return state_.prepareClose(stamp(), expected);
        }
        SessionState state_;
        Model source_;
        std::unique_ptr<editing::EditHistory> history_;
    };
    class OtherSession final : public IEditSession
    {
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
        assert(edits->undo() && source.value == 0);
        assert(checkpoint.clean(edits->view()->snapshot.current, {}));
        assert(edits->redo() && source.value == 1);
        assert(edit(*edits, source, 0)); // Same bytes, different history state, still dirty.
        assert(!checkpoint.clean(edits->view()->snapshot.current, {}));
        const auto current = edits->view()->snapshot.current;
        assert(checkpoint.accept(current, {}, {baseline, {}, {3}}));
        assert(!checkpoint.accept(current, {}, {current, {}, {2}}));
        assert(checkpoint.accept(current, {}, {current, {}, {4}}));
        assert(checkpoint.clean(current, {}));
        assert(edits->clear() && checkpoint.clean(edits->view()->snapshot.current, {}));
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
        assert(edits->close());
    }
    void slotsAndPermits()
    {
        SessionStore store{1};
        auto reservation = store.reserve<FakeSession>({"test.fake"}, contracts::CodeLease::builtin());
        assert(reservation && store.size() == 0);
        const auto first = reservation->id();
        assert(!store.describe(first));
        auto session = std::make_unique<FakeSession>(first);
        assert(store.prepare(*reservation, session) && !session && !store.describe(first));
        assert(store.publish(*reservation) && store.size() == 1);
        auto key = store.key<FakeSession>(first);
        assert(key && !store.key<OtherSession>(first));
        SessionStore other{1};
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
        auto second = store.reserve<FakeSession>({"test.fake"}, contracts::CodeLease::builtin());
        assert(second && second->id().slot == first.slot && second->id().generation != first.generation);
        session = std::make_unique<FakeSession>(second->id());
        assert(store.prepare(*second, session) && store.publish(*second));
        assert(!store.key<FakeSession>(first) && !store.close(*close));
        auto next = store.prepareClose(store.describe(second->id())->current);
        assert(next && store.close(*next));
    }
    void candidatesAndCode()
    {
        SessionStore store{1};
        assert(!store.reserve<FakeSession>({"test.fake"}, contracts::CodeLease::plugin({})));
        std::vector<int> order;
        {
            auto code = std::shared_ptr<const void>(new int{}, [&order](const void* p) {
                order.push_back(3);
                delete static_cast<const int*>(p);
            });
            auto reservation = store.reserve<FakeSession>({"test.fake"}, contracts::CodeLease::plugin(code));
            code.reset();
            assert(reservation && !store.publish(*reservation));
            assert(!store.reserve<FakeSession>({"test.fake"}, contracts::CodeLease::builtin()));
            auto invalid = std::make_unique<FakeSession>(SessionId{999, 0, 1});
            assert(!store.prepare(*reservation, invalid) && invalid && store.size() == 0);
            auto candidate = std::make_unique<FakeSession>(reservation->id(), &order);
            assert(store.prepare(*reservation, candidate));
            assert(order.empty() && store.size() == 0);
        }
        assert((order == std::vector<int>{1, 2, 3}) && store.size() == 0);
    }
}
int main()
{
    checkpointAndHistory();
    slotsAndPermits();
    candidatesAndCode();
}
