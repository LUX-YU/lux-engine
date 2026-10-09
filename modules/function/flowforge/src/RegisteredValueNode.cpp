#include <lux/engine/flowforge/FlowNodeCatalog.hpp>
#include <lux/engine/flowforge/graph/NodeBase.hpp>

#include <algorithm>

namespace lux::flowforge
{
    namespace
    {
        class RegisteredValueNode final : public Node
        {
        public:
            RegisteredValueNode(
                std::shared_ptr<const FlowNodeType> type,
                FlowNodePayload payload,
                std::vector<FlowPinDeclaration> pins
            ) noexcept
                : Node(ENodeOperation::REGISTERED_VALUE), type_(std::move(type)), payload_(std::move(payload)),
                  declarations_(std::move(pins))
            {
                setName(type_->identity().canonical_name);
                for (const auto& declaration : declarations_)
                {
                    const DataPinInfo info{declaration.name, declaration.type};
                    if (declaration.direction == graph::EPinDirection::INPUT)
                    {
                        pins_.push_back(std::make_unique<DataInPin>(this, info));
                    }
                    else
                    {
                        pins_.push_back(std::make_unique<DataOutPin>(this, info, declaration.name));
                    }
                }
            }

            const FlowNodeType* registeredType() const noexcept override
            {
                return type_.get();
            }

            const FlowNodePayload* registeredPayload() const noexcept override
            {
                return &payload_;
            }

            graph::PinSemanticId registeredPinSemantic(const Pin& pin) const noexcept override
            {
                const auto found = std::ranges::find(pins_, &pin, &std::unique_ptr<Pin>::get);
                return found == pins_.end() ? graph::PinSemanticId{} : declarations_[found - pins_.begin()].semantic;
            }

        private:
            std::shared_ptr<const FlowNodeType> type_;
            FlowNodePayload payload_;
            std::vector<FlowPinDeclaration> declarations_;
            std::vector<std::unique_ptr<Pin>> pins_;
        };
    } // namespace

    FlowForgeResult<std::unique_ptr<Node>> createFlowValueNode(
        std::shared_ptr<const FlowNodeType> type,
        FlowNodePayload payload
    ) noexcept
    {
        if (!type)
        {
            return cxx::unexpected(FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "missing node definition"});
        }
        auto pins = type->describePins(payload);
        if (!pins)
        {
            return cxx::unexpected(std::move(pins.error()));
        }
        return std::unique_ptr<Node>(new RegisteredValueNode(std::move(type), std::move(payload), std::move(*pins)));
    }
} // namespace lux::flowforge
