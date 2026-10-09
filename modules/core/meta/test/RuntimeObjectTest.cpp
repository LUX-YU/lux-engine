#include <lux/engine/meta/RuntimeObject.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

namespace
{
    void require(bool value, const char* message) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::abort();
        }
    }

    struct alignas(64) LargeValue
    {
        std::uint64_t value[16];
    };

    struct ForeignValue
    {
        std::string text{"foreign"};
    };
} // namespace

int main()
{
    using lux::meta::ERuntimeObjectError;
    using lux::meta::RuntimeObject;
    static_assert(!std::is_copy_constructible_v<RuntimeObject>);
    static_assert(std::is_nothrow_move_constructible_v<RuntimeObject>);

    RuntimeObject empty;
    require(!empty && empty.type() == nullptr, "empty owner");
    auto empty_copy = empty.clone();
    require(empty_copy && !*empty_copy, "empty clone is successful empty");
    RuntimeObject view{std::string_view{"without registration"}};
    require(view.get<std::string_view>() == "without registration", "typed view needs no registry");
    auto view_copy = view.clone();
    require(view_copy && view_copy->get<std::string_view>() == view.get<std::string_view>(), "view copy");

    RuntimeObject integer{std::int64_t{42}};
    const auto& constant = integer;
    auto integer_copy = constant.clone();
    require(integer_copy && integer_copy->get<std::int64_t>() == 42, "const clone");
    integer_copy->get<std::int64_t>() = 9;
    require(integer.get<std::int64_t>() == 42, "independent value");
    RuntimeObject moved{std::move(*integer_copy)};
    require(!*integer_copy && moved.get<std::int64_t>() == 9, "move leaves empty source");
    *integer_copy = std::move(moved);
    require(!moved && integer_copy->get<std::int64_t>() == 9, "move assignment");
    auto zero = RuntimeObject::defaultOf(lux::meta::ref_type_of_v<std::int64_t>);
    require(zero && zero->get<std::int64_t>() == 0, "trivial default");
    auto large = RuntimeObject::defaultOf(lux::meta::ref_type_of_v<LargeValue>);
    require(large.has_value(), "aligned heap default");
    require(reinterpret_cast<std::uintptr_t>(large->data()) % alignof(LargeValue) == 0, "heap alignment");
    static_cast<LargeValue*>(large->data())->value[15] = 73;
    auto large_copy = large->clone();
    require(large_copy && static_cast<const LargeValue*>(large_copy->data())->value[15] == 73, "heap clone");
    require(large_copy->data() != large->data(), "heap clone owns separate allocation");

    auto invalid_type = lux::meta::ref_type_of_v<int>;
    invalid_type.alignment = 3;
    auto invalid = RuntimeObject::defaultOf(invalid_type);
    require(!invalid && invalid.error() == ERuntimeObjectError::INVALID_TYPE, "invalid alignment");
    invalid_type.alignment = alignof(int);
    invalid_type.size = 0;
    invalid = RuntimeObject::defaultOf(invalid_type);
    require(!invalid && invalid.error() == ERuntimeObjectError::INVALID_TYPE, "zero size");
    auto unavailable = RuntimeObject::defaultOf(lux::meta::ref_type_of_v<std::string>);
    require(!unavailable && unavailable.error() == ERuntimeObjectError::DEFAULT_UNAVAILABLE, "nontrivial default");
    auto missing = RuntimeObject::create(static_cast<const lux::meta::RefClass*>(nullptr));
    require(!missing && missing.error() == ERuntimeObjectError::INVALID_TYPE, "missing class");
    auto missing_string = RuntimeObject::create(std::string{"before registry"});
    require(!missing_string && missing_string.error() == ERuntimeObjectError::INVALID_TYPE, "missing registry");

    // Foreign callbacks deliberately throw; these are containment tests, not Lux domain error production.
    lux::meta::RefClass cls{};
    cls.type = lux::meta::ref_type_of_v<ForeignValue>;
    cls.type.ptr = &cls;
    auto no_constructor = RuntimeObject::create(&cls);
    require(
        !no_constructor && no_constructor.error() == ERuntimeObjectError::CONSTRUCTION_UNAVAILABLE,
        "no constructor"
    );
    cls.destruct = [](void* value) { static_cast<ForeignValue*>(value)->~ForeignValue(); };
    cls.construct = [](void*) { throw 7; };
    auto construction_failed = RuntimeObject::create(&cls);
    require(
        !construction_failed && construction_failed.error() == ERuntimeObjectError::CONSTRUCTION_FAILURE,
        "foreign construction"
    );
    cls.construct = [](void* value) { new (value) ForeignValue(); };
    {
        auto original = RuntimeObject::create(&cls);
        require(original.has_value(), "foreign construct");
        auto no_copy = original->clone();
        require(!no_copy && no_copy.error() == ERuntimeObjectError::COPY_UNAVAILABLE, "no copy constructor");
        cls.copy_construct = [](void*, const void*) { throw 8; };
        auto failed = original->clone();
        require(!failed && failed.error() == ERuntimeObjectError::COPY_FAILURE, "foreign copy");
        require(static_cast<const ForeignValue*>(original->data())->text == "foreign", "failed copy retains original");
        lux::meta::ref_class_func_gen<ForeignValue>(cls);
        auto copied = original->clone();
        require(copied && static_cast<const ForeignValue*>(copied->data())->text == "foreign", "typed callback copy");
    }
    for (unsigned cycle = 0; cycle != 2; ++cycle)
    {
        lux::meta::meta_module_init();
        {
            auto text = RuntimeObject::create(std::string(200, 'x'));
            require(text.has_value(), "string factory after prior missing registry");
            auto copied = text->clone();
            require(copied.has_value(), "real string clone");
            auto& source = *static_cast<std::string*>(text->data());
            auto& target = *static_cast<std::string*>(copied->data());
            require(target == source && target.data() != source.data(), "string own allocation");
            source[0] = 'y';
            require(target[0] == 'x', "string independent");
        }
        lux::meta::meta_module_deinit();
    }
    integer.reset();
    require(!integer, "reset");
    std::puts("PASS: nullable, typed, aligned, default, copy, foreign containment and registry lifetime");
}
