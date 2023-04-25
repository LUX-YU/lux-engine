#pragma once
#include "extended_type_traits.hpp"

namespace lux::engine::platform
{
    /**
     * @brief A compile time string, which is guarantee the last character is \0
     * @tparam Chars The characters sequence
     */
    template<char... Chars>
    class ct_string
    {
    private:
        template<char... OtherChars>
        static constexpr auto __concat_chars() 
            -> ct_string<Chars..., OtherChars...>;

        template<char... OtherChars>
        static constexpr auto __concat_string(ct_string<OtherChars...>) 
            -> ct_string<Chars..., OtherChars...>;
    public:
        // c style length
        static constexpr size_t             len = sizeof...(Chars);
        // c style character array
        static constexpr char               str[len + 1] = {Chars..., 0};
        // c style std::string_view
        static constexpr std::string_view   view{str, len + 1};

        template<char... OtherChars> 
        using concat_chars = decltype(__concat_chars<OtherChars...>());

        template<class T> 
        using concat_string = decltype(__concat_string(T{}));
    };

    template<class _CTStr>
    concept ct_string_type = is_template_of_v<ct_string, _CTStr>;

    template<ct_string_type _CTStr1, ct_string_type _CTStr2>
    struct concat_string
    {
        using type = typename _CTStr1::template concat_string<_CTStr2>;
    };

    template<ct_string_type _CTStr1, ct_string_type _CTStr2>
    using concat_string_t = typename concat_string<_CTStr1, _CTStr2>::type;

    /**
     * @brief concat any number of compile time string
     * 
     * @tparam _CT_Strs requires ct_string_type
     */
    template<class... _CT_Strs>
    struct concat_strings;

    template<ct_string_type T ,ct_string_type U>
    struct concat_strings<T , U> // last two
    {
        using type = concat_string_t<T, U>;
    };

    template<ct_string_type T>
    struct concat_strings<T> // last one
    {
        using type = T;
    };

    template<ct_string_type _Current_Str, ct_string_type... Lasts>
    struct concat_strings<_Current_Str, Lasts...> : private concat_strings<Lasts...>
    {
        using _Base_Concat = concat_strings<Lasts...>;
        using type = concat_string_t<_Current_Str, typename _Base_Concat::type>;
    };

    template<ct_string_type... _CTStrs> using concat_strings_t = typename concat_strings<_CTStrs...>::type;

    /**
     * @brief generate compile time string from integer
     * 
     *  depend on template <size_t _Ty> integer_to_sequence.
     * 
     *  for example:
     *    ctstring_index_generator<18645>::type is ct_string<'1','8','6','4','5'>
     * 
     * @tparam _Ty 
     */
    template<size_t _Ty>
    class ctstring_index_generator
    {
    private:
        template<char... num>
        static constexpr auto _generate(std::integer_sequence<char, num...>)
            ->ct_string<(num + 48)...>;
    public:
        using type = decltype(_generate(integer_to_sequence_t<_Ty>{}));
    };

    template<size_t _Ty> using ctstring_index_generator_t = typename ctstring_index_generator<_Ty>::type;
}