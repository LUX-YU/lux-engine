#pragma once
#include <string_view>
#include <array>
#include <utility>

namespace lux::engine::platform
{
    template<template<auto...> class T, class U>
    class is_template_of
    {
    private:
        template<auto... _TPack>
        static constexpr auto _d(T<_TPack...>*) -> std::true_type;
        static constexpr auto _d(...) -> std::false_type;
    public:
        static constexpr bool value = decltype(_d((U*)nullptr))::value;
    };

    template<template<auto...> class T, class U> 
    constexpr inline bool is_template_of_v = is_template_of<T, U>::value;

    /**
     * @brief convert a integer to integer sequence(std::integer_sequence<char, ...>).
     * for example :
     *    integer_to_sequence<18645>::type is std::integer_sequence<char, 1, 8, 6, 4, 5>;
     * 
     * @tparam _Ty the integer will be convert to sequence by digits
     */
    template <size_t _Ty>
    class integer_to_sequence
    {
    private:
        template <size_t num, size_t next>
        struct convertor;

        template <size_t num>
        struct convertor<num, 0>
        {
            using type = std::integer_sequence<char, num>;
        };

        template <size_t num, size_t next = num / 10>
        struct convertor : private convertor<next>
        {
            using parent_type = convertor<next>;
            using parent_seq_type = typename convertor<next>::type;

            template <char... pseq>
            static constexpr auto concat(std::integer_sequence<char, pseq...>)
            {
                return std::integer_sequence<char, pseq..., num % 10>{};
            };

            using type = decltype(concat(parent_seq_type{}));
        };

    public:
        using type = typename convertor<_Ty>::type;
    };

    template<size_t _Ty> using integer_to_sequence_t = typename integer_to_sequence<_Ty>::type;
} // namespace lux::engine::platform
