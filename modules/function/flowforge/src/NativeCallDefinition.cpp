#include <lux/engine/flowforge/NativeCallDefinition.hpp>

namespace lux::flowforge
{
    NativeCallDefinition::NativeCallDefinition(const meta::RefInvokable& source, const meta::RefType* receiver)
        : signature_(source)
    {
        // Allocate every string slot before publishing any string_view, including short strings.
        const auto type_count = 1U + source.parameters.size() + (receiver ? 1U : 0U);
        records_.reserve(type_count);
        strings_.resize(4U + source.parameters.size() * 3U + (receiver ? 1U : 0U) + type_count * 4U);
        std::size_t next{};
        const auto own = [&](std::string_view value) -> std::string_view
        {
            auto& storage = strings_[next++];
            storage.assign(value);
            return storage;
        };
        const auto own_type = [&](meta::RefType& type)
        {
            type.name = own(type.name);
            const bool has_record = meta::p_is_base_record(type.qtype) && type.ptr != nullptr;
            if (!has_record)
            {
                type.ptr = nullptr;
                return;
            }
            const auto& original = *static_cast<const meta::RefClass*>(type.ptr);
            auto& record = records_.emplace_back();
            record.name = own(original.name);
            record.full_name = own(original.full_name);
            record.hash = original.hash;
            record.parent_chain = original.parent_chain;
            record.is_abstract = original.is_abstract;
            record.construct = original.construct;
            record.destruct = original.destruct;
            record.copy = original.copy;
            record.copy_construct = original.copy_construct;
            record.move = original.move;
            record.move_construct = original.move_construct;
            record.construct_symbol = own(original.construct_symbol);
            record.destruct_symbol = own(original.destruct_symbol);
            record.type = original.type;
            record.type.name = record.full_name;
            record.type.ptr = &record;
            type.ptr = &record;
        };
        signature_.name = own(source.name);
        signature_.full_name = own(source.full_name);
        signature_.type_signature = own(source.type_signature);
        own_type(signature_.return_type);
        for (auto& parameter : signature_.parameters)
        {
            parameter.name = own(parameter.name);
            parameter.value_type_name = own(parameter.value_type_name);
            own_type(parameter.type);
        }
        if (receiver)
        {
            receiver_ = *receiver;
            own_type(*receiver_);
        }
    }

    NativeCallDefinition::~NativeCallDefinition() = default;

    NativeCallDefinition::Result NativeCallDefinition::create(
        const meta::RefInvokable& source,
        object::CodeLease code,
        const meta::RefType* receiver
    ) noexcept
    {
        const bool has_identity = !source.name.empty();
        const bool has_code = code.valid();
        const bool is_invalid_definition = !has_identity || !has_code;
        if (is_invalid_definition)
        {
            return cxx::unexpected(FlowForgeFailure{
                EFlowForgeError::INVALID_DESCRIPTION,
                "native call requires a name and an explicit code lease"
            });
        }
        // The stable Object provider keeps code alive outside the signature's deleter/control block.
        // This also covers a definition allocated by a statically linked copy inside an extension DLL.
        auto value = std::shared_ptr<const NativeCallDefinition>(new NativeCallDefinition(source, receiver));
        return object::pinCodeOwner(std::move(code), std::move(value));
    }

    const meta::RefInvokable& NativeCallDefinition::signature() const noexcept
    {
        return signature_;
    }

    const meta::RefType* NativeCallDefinition::receiver() const noexcept
    {
        return receiver_ ? &*receiver_ : nullptr;
    }
} // namespace lux::flowforge
