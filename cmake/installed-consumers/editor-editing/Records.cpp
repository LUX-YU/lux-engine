#include "Consumer.hpp"
#include <new>
#include <optional>
#include <utility>
#include <vector>
using namespace lux::editor::editing;
namespace consumer::records
{
    struct Record final
    {
        int key{}, value{};
    };
    struct Model final : ConsumerState
    {
        std::vector<Record> records{{7, 42}};
        std::optional<int> selected{7};
    };
    class Remove final : public EditOperation
    {
        class Plan final : public PreparedEdit
        {
            Model& model_;
            std::vector<Record> image_;
            std::optional<int> selected_;

        public:
            Plan(Model& model, std::vector<Record> image, std::optional<int> selected) noexcept
                : model_(model), image_(std::move(image)), selected_(selected)
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
                model_.records.swap(image_);
                std::swap(model_.selected, selected_);
            }
            void publish(const CommitInfo&) noexcept override
            {
                assert(model_.records.empty() == !model_.selected.has_value());
            }
        };
        Model& model_;
        const StateId base_;

    public:
        Remove(Model& model, StateId base) noexcept : model_(model), base_(base)
        {
        }
        ~Remove() noexcept override
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
            return "remove record";
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            return sizeof(Remove);
        }
        EditResult<PreparedEditPtr> prepare(const ApplyContext& context, EditPreparationBudget& budget)
            const noexcept override
        {
            const auto forward = context.direction == EDirection::FORWARD;
            const bool valid =
                forward ? model_.records.size() == 1U && model_.records[0].key == 7 : model_.records.empty();
            if (!valid)
            {
                return lux::cxx::unexpected(makeEditFailure(EEditError::PRECONDITION_FAILED));
            }
            if (auto reserved = budget.reserve(sizeof(Plan) + 128U); !reserved)
            {
                return lux::cxx::unexpected(reserved.error());
            }
            {
                std::vector<Record> records;
                if (!forward)
                {
                    records.push_back({7, 42});
                }
                return PreparedEditPtr(
                    new Plan(model_, std::move(records), forward ? std::nullopt : std::optional<int>(7))
                );
            }

        }
    };
} // namespace consumer::records
extern "C" ED1_RECORDS_API ConsumerReport runRecordsConsumer()
{
    consumer::records::Model model;
    auto made = EditHistory::create({{8U, 8192U, 8192U, 64U}, {&model, ConsumerState::notice}});
    assert(made);
    auto history = std::move(*made);
    const auto identity = history->id();
    EditOperationPtr operation = std::make_unique<consumer::records::Remove>(model, history->view()->snapshot.current);
    assert(history->execute(operation) && !operation && model.records.empty() && !model.selected);
    assert(history->undo() && model.records[0].value == 42 && model.selected == 7);
    assert(history->redo() && model.records.empty() && !model.selected);
    assert(history->close());
    assert(model.operations == 1U && model.plans == 3U && model.notices == 4U);
    return {identity, model.operations, model.plans, model.notices, 42U};
}
