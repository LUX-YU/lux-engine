#pragma once
#include <string_view>
#include <array>
#include <utility>

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

    template<class _CTStr1, class _CTStr2>
    struct concat_string
    {
        using type = typename _CTStr1::template concat_string<_CTStr2>;
    };

    template<class _CTStr1, class _CTStr2>
    using concat_string_t = typename concat_string<_CTStr1, _CTStr2>::type;

    /**
     * @brief convert a integer to integer sequence(std::integer_sequence<uint8_t, ...>).
     * for example :
     *    integer_to_sequence<18645>::type is std::integer_sequence<uint8_t, 1, 8, 6, 4, 5>;
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
            using type = std::integer_sequence<uint8_t, num>;
        };

        template <size_t num, size_t next = num / 10>
        struct convertor : convertor<next>
        {
            using parent_type = convertor<next>;
            using parent_seq_type = typename convertor<next>::type;

            template <uint8_t... pseq>
            static constexpr auto concat(std::integer_sequence<uint8_t, pseq...>)
            {
                return std::integer_sequence<uint8_t, pseq..., num % 10>{};
            };

            using type = decltype(concat(parent_seq_type{}));
        };

    public:
        using type = typename convertor<_Ty>::type;
    };

    template<size_t _Ty> using integer_to_sequence_t = typename integer_to_sequence<_Ty>::type;

    /**
     * @brief generate compile time string from integer
     *  depend on template <size_t _Ty> integer_to_sequence.
     *  for example:
     *    ctstring_index_generator<18645>::type is ct_string<'1','8','6','4','5'>
     * 
     * @tparam _Ty 
     */
    template<size_t _Ty>
    class ctstring_index_generator
    {
    private:
        template<uint8_t... num>
        static constexpr auto _generate(std::integer_sequence<uint8_t, num...>)
            ->ct_string<(num + 48)...>;

    public:
        using type = decltype(_generate(integer_to_sequence_t<_Ty>{}));
    };
} // namespace lux::engine::platform
