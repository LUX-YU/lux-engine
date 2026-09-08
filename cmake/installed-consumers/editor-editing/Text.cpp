#include "Consumer.hpp"
#include <new>
#include <utility>
using namespace lux::editor::editing;
namespace consumer::text
{
    struct Model final : ConsumerState
    {
        std::string content{"alpha"};
    };
    class Replace final : public EditOperation
    {
        class Plan final : public PreparedEdit
        {
            Model& model_;
            std::string prepared_;

        public:
            Plan(Model& model, std::string image) noexcept : model_(model), prepared_(std::move(image))
            {
            }
            ~Plan() noexcept override
            {
                ++model_.plans;
            }
            EEditEffect effect() const noexcept override
            {
                return EEditEffect::CHANGE;
            }

        private:
            void apply() noexcept override
            {
                model_.content.swap(prepared_);
            }
            void publish(const CommitInfo& info) noexcept override
            {
                assert(info.label == "replace");
            }
        };
        Model& model_;
        const StateId base_;

    public:
        Replace(Model& model, StateId base) noexcept : model_(model), base_(base)
        {
        }
        ~Replace() noexcept override
        {
            ++model_.operations;
        }
        HistoryId historyId() const noexcept override
        {
            return base_.history;
        }
        StateId baseState() const noexcept override
        {
            return base_;
        }
        std::string_view label() const noexcept override
        {
            return "replace";
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            return sizeof(Replace);
        }
        EditResult<PreparedEditPtr> prepare(const ApplyContext& context, EditPreparationBudget& budget)
            const noexcept override
        {
            const auto forward = context.direction == EDirection::FORWARD;
            if (model_.content != (forward ? "alpha" : "beta"))
            {
                return lux::cxx::unexpected(makeEditFailure(EEditError::PRECONDITION_FAILED));
            }
            if (auto reserved = budget.reserve(sizeof(Plan) + 64U); !reserved)
            {
                return lux::cxx::unexpected(reserved.error());
            }
            try
            {
                return PreparedEditPtr(new Plan(model_, forward ? "beta" : "alpha"));
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(makeEditFailure(EEditError::ALLOCATION_FAILURE));
            }
        }
    };
} // namespace consumer::text
extern "C" ED1_EXPORT ConsumerReport runTextConsumer()
{
    consumer::text::Model model;
    auto made = EditHistory::create({{8U, 8192U, 8192U, 64U}, {&model, ConsumerState::notice}, true});
    assert(made);
    auto history = std::move(*made);
    const auto identity = history->id();
    EditOperationPtr operation = std::make_unique<consumer::text::Replace>(model, history->view()->snapshot.current);
    assert(history->execute(operation) && !operation && model.content == "beta");
    assert(history->undo() && model.content == "alpha" && history->view()->snapshot.clean);
    assert(history->redo() && model.content == "beta");
    assert(history->close());
    assert(model.operations == 1U && model.plans == 3U && model.notices == 4U);
    return {identity, model.operations, model.plans, model.notices, 4U};
}
