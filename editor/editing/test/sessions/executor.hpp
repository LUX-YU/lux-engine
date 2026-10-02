#pragma once

// These checks use two real execution objects against one real History. No Session gate can
// accidentally hide a missing History gate here.
namespace
{
    template <class T>
    concept HasHistoryMutations = requires(T& value, editing::EditOperationPtr& operation) {
        value.execute(operation);
    } || requires(T& value) { value.undo(); } || requires(T& value) { value.redo(); } ||
        requires(T& value) { value.clear(); } || requires(T& value) { value.close(); };
    static_assert(!HasHistoryMutations<editing::EditHistory>);

    struct ExecutionProbe final
    {
        editing::EditHistory* history{};
        editing::EditExecutor second;
        std::size_t preparation{}, publication{}, reclamation{}, notices{};

        void check() noexcept
        {
            const auto phase = history->view()->phase;
            const auto result = second.clear(*history);
            assert(!result && result.error().code == editing::EEditError::BUSY);
            assert(history->view()->phase == phase);
            const auto undone = second.undo(*history);
            assert(!undone && undone.error().code == editing::EEditError::BUSY);
        }
    };
    class ProbedOperation final : public editing::EditOperation
    {
        class Plan final : public editing::PreparedEdit
        {
            ExecutionProbe& probe_;
            int& value_;
            int next_;

        public:
            Plan(ExecutionProbe& probe, int& value, int next) : probe_(probe), value_(value), next_(next) {}
            ~Plan() noexcept override
            {
                ++probe_.reclamation;
                probe_.check();
            }
            editing::EEditEffect effect() const noexcept override
            {
                return editing::EEditEffect::CHANGE;
            }

        private:
            void apply() noexcept override
            {
                value_ = next_;
            }
            void publish(const editing::CommitInfo& commit) noexcept override
            {
                ++probe_.publication;
                assert(probe_.history->view()->snapshot.current == commit.to);
                probe_.check();
            }
        };
        ExecutionProbe& probe_;
        editing::StateId base_;
        int& value_;
        int before_, after_;

    public:
        ProbedOperation(ExecutionProbe& probe, int& value, int next)
            : probe_(probe), base_(probe.history->view()->snapshot.current), value_(value), before_(value), after_(next)
        {}
        ~ProbedOperation() noexcept override
        {
            ++probe_.reclamation;
            if (!probe_.history->view()->snapshot.closed)
                probe_.check();
        }
        editing::HistoryId historyId() const noexcept override { return base_.history; }
        editing::StateId baseState() const noexcept override { return base_; }
        std::string_view label() const noexcept override { return "probe"; }
        std::size_t retainedBytesUpperBound() const noexcept override { return sizeof(*this); }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext& context,
            editing::EditPreparationBudget& budget
        ) const noexcept override
        {
            ++probe_.preparation;
            probe_.check();
            auto reserved = budget.reserve(sizeof(Plan));
            if (!reserved)
                return lux::cxx::unexpected(reserved.error());
            return std::make_unique<Plan>(
                probe_, value_, context.direction == editing::EDirection::FORWARD ? after_ : before_
            );
        }
    };

    void executorContracts()
    {
        ExecutionProbe probe;
        auto created = editing::EditHistory::create({
            {2, 4096, 1024, 64},
            {&probe, [](void* opaque, const editing::HistoryNotice& notice) noexcept {
                auto& observer = *static_cast<ExecutionProbe*>(opaque);
                ++observer.notices;
                if (notice.kind != editing::EHistoryEvent::CLOSED)
                    observer.check();
            }}
        });
        assert(created);
        auto& log = **created;
        probe.history = &log;
        editing::EditExecutor first;
        int value{};
        editing::EditOperationPtr operation = std::make_unique<ProbedOperation>(probe, value, 7);
        const auto initial = log.view()->snapshot.current;
        assert(first.execute(log, operation) && !operation && value == 7);
        const auto changed = log.view()->snapshot.current;
        assert(probe.second.undo(log) && value == 0 && log.view()->snapshot.current == initial);
        assert(first.redo(log) && value == 7 && log.view()->snapshot.current == changed);
        std::jthread foreign([&] {
            const auto result = probe.second.undo(log);
            assert(!result && result.error().code == editing::EEditError::WRONG_THREAD);
        });
        foreign.join();
        assert(value == 7 && log.view()->snapshot.current == changed);
        assert(first.clear(log) && log.view()->snapshot.current == changed);
        assert(probe.preparation == 3 && probe.publication == 3 && probe.reclamation == 4 && probe.notices == 4);
        assert(first.close(log) && probe.second.close(log) && probe.notices == 5);
        assert(log.view()->snapshot.closed);
    }
}
