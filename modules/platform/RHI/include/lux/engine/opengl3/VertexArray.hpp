#pragma once
#include <glad/glad.h>
#include <tuple>
#include <utility>
#include "ValueTypes.hpp"

namespace lux::gapi::opengl
{
    /**
     * Specifies the data type of each component in the array. 
     * The symbolic constants 
     *  GL_BYTE, 
     *  GL_UNSIGNED_BYTE, 
     *  GL_SHORT, 
     *  GL_UNSIGNED_SHORT, 
     *  GL_INT, and 
     *  GL_UNSIGNED_INT 
     * are accepted by glVertexAttribPointer and glVertexAttribIPointer. 
     * 
     * Additionally 
     *  GL_HALF_FLOAT, 
     *  GL_FLOAT, 
     *  GL_DOUBLE, 
     *  GL_FIXED, 
     *  GL_INT_2_10_10_10_REV, 
     *  GL_UNSIGNED_INT_2_10_10_10_REV and 
     *  GL_UNSIGNED_INT_10F_11F_11F_REV 
     * are accepted by glVertexAttribPointer. 
     * 
     * GL_DOUBLE is also accepted by glVertexAttribLPointer and is the only 
     * token accepted by the type parameter for that function. The initial value is GL_FLOAT.
    */
    template<DataType dtype> struct TAttrFuncMap;
#define ATTR_FUNC_MAP(TYPE, FUNC)\
    template<> struct TAttrFuncMap<TYPE>{static constexpr auto& func = FUNC ;};
    ATTR_FUNC_MAP(DataType::BYTE,                           glVertexAttribIPointer);
    ATTR_FUNC_MAP(DataType::UNSIGNED_BYTE,                  glVertexAttribIPointer);
    ATTR_FUNC_MAP(DataType::SHORT,                          glVertexAttribIPointer);
    ATTR_FUNC_MAP(DataType::UNSIGNED_SHORT,                 glVertexAttribIPointer);
    ATTR_FUNC_MAP(DataType::INT,                            glVertexAttribIPointer);
    ATTR_FUNC_MAP(DataType::UNSIGNED_INT,                   glVertexAttribIPointer);

    ATTR_FUNC_MAP(DataType::HALF_FLOAT,                     glVertexAttribPointer);
    ATTR_FUNC_MAP(DataType::FLOAT,                          glVertexAttribPointer);
    ATTR_FUNC_MAP(DataType::FIXED,                          glVertexAttribPointer);
    ATTR_FUNC_MAP(DataType::INT_2_10_10_10_REV,             glVertexAttribPointer);
    ATTR_FUNC_MAP(DataType::UNSIGNED_INT_2_10_10_10_REV,    glVertexAttribPointer);
    ATTR_FUNC_MAP(DataType::UNSIGNED_INT_10F_11F_11F_REV,   glVertexAttribPointer);

    ATTR_FUNC_MAP(DataType::DOUBLE,                         glVertexAttribLPointer);

    // TODO TVertexDataSlice and TVertexDataStruct are graphic api independent, move these class out of the opengl
    template<GLint Number, DataType Type, GLboolean Normalize = GL_FALSE>
    class TVertexDataSlice
    {
    public:
        static constexpr GLint      number      = Number;
        static constexpr GLsizei    size        = number * TDataTypeMap<Type>::size;
        static constexpr DataType   type        = Type;
        static constexpr GLenum     type_value  = static_cast<GLenum>(Type);
        static constexpr GLboolean  normalize   = Normalize;

        template<GLuint Index, GLsizei Stride, GLuint Offset>
        static void sAttributePointer()
        {
            TAttrFuncMap<Type>::func(Index, number, type_value, normalize, Stride, reinterpret_cast<void*>(Offset));
        }
    }; 

    // predefined data slice
    using VertexDataSlice2ui  = TVertexDataSlice<2, DataType::UNSIGNED_INT>;
    using VertexDataSlice3ui  = TVertexDataSlice<3, DataType::UNSIGNED_INT>;
    using VertexDataSlice4ui  = TVertexDataSlice<4, DataType::UNSIGNED_INT>;

    using VertexDataSlice2i   = TVertexDataSlice<2, DataType::INT>;
    using VertexDataSlice3i   = TVertexDataSlice<3, DataType::INT>;
    using VertexDataSlice4i   = TVertexDataSlice<4, DataType::INT>;

    using VertexDataSlice2f   = TVertexDataSlice<2, DataType::FLOAT>;
    using VertexDataSlice3f   = TVertexDataSlice<3, DataType::FLOAT>;
    using VertexDataSlice4f   = TVertexDataSlice<4, DataType::FLOAT>;

    using VertexDataSlice2d   = TVertexDataSlice<2, DataType::DOUBLE>;
    using VertexDataSlice3d   = TVertexDataSlice<3, DataType::DOUBLE>;
    using VertexDataSlice4d   = TVertexDataSlice<4, DataType::DOUBLE>;

    template<class T> 
    class is_vertex_data_slice
    {
    private:
        static constexpr auto check(...) -> std::false_type;
        template<GLint _TP1, DataType _TP2, GLboolean _TP3>
        static constexpr auto check(TVertexDataSlice<_TP1,_TP2,_TP3>&&) -> std::true_type;

    public:
        static constexpr bool value = decltype(check(T{}))::value;
    };

    template<class... Args>
    struct is_vertex_data_slice_pack
    {
        static constexpr bool value = (is_vertex_data_slice<Args>::value && ...);
    };

    template<class... Args>
    class TVertexDataStruct
    {
    public:
        static_assert(is_vertex_data_slice_pack<Args...>::value, "not all type in parameter pack are TVertexDataSlice.");
        static constexpr GLint          size = (Args::size + ...);
        static constexpr std::size_t    slice_number = sizeof...(Args);

        using slice_type_tuple = std::tuple<Args...>;
    private:
        template<size_t I>
        static constexpr std::enable_if_t<I == 0, GLuint> __cal_offset()
        {
            return 0;
        }

        template<size_t I>
        static constexpr std::enable_if_t< (I > 0), GLuint> __cal_offset()
        {
            using slice_type = std::tuple_element_t<I - 1, slice_type_tuple>;
            return __cal_offset<I - 1>() + slice_type::size;
        }

        template<size_t... I>
        static constexpr std::array<GLuint, slice_number> __offset_array(std::index_sequence<I...>)
        {
            return std::array<GLuint, slice_number>{__cal_offset<I>()...};
        }

        static constexpr auto offset_array = __offset_array(std::make_index_sequence<slice_number>());

        template<size_t... I>
        static constexpr void __attribute_pointer_all(std::index_sequence<I...>)
        {
            (std::tuple_element_t<I, slice_type_tuple>::template sAttributePointer<I, size, offset_array[I]>(), ...);
        }
    public:
        template<size_t I>
        static constexpr GLuint offset_of_slice()
        {
            return offset_array[I];
        }

        template<size_t I>
        static constexpr void sAttributePointerbyIndex()
        {
            std::tuple_element_t<I, slice_type_tuple>::template sAttributePointer<I, size, offset_array[I]>();
        }

        static constexpr void sAttributePointerAll()
        {
            __attribute_pointer_all(std::make_index_sequence<slice_number>());
        }
    };

    template<class T> 
    class is_vertex_data
    {
    private:
        static constexpr auto check(...) -> std::false_type;
        template<class... Args>
        static constexpr auto check(TVertexDataStruct<Args...>&&) -> std::true_type;

    public:
        static constexpr bool value = decltype(check(T{}))::value;
    };

    class VertexArray
    {
    public:
        VertexArray(GLsizei number)
        :_num(number){
            if(_num > 0)
                glGenVertexArrays(_num, &_vao);
        }

        VertexArray()
        :_num(1){
            glGenVertexArrays(_num, &_vao);
        }

        VertexArray(const VertexArray&) = delete;
        VertexArray& operator=(const VertexArray&) = delete;

        VertexArray(VertexArray&& other) noexcept
        {
            _num = other._num;
            _vao = other._vao;
            other._num = 0;
        }

        VertexArray& operator=(VertexArray&& other) noexcept
        {
            release();
            _num = other._num;
            _vao = other._vao;
            other._num = 0;
            return *this;
        }

        ~VertexArray()
        {
            release();
        }

        void release()
        {
            if(_num > 0)
            {
                glDeleteVertexArrays(_num, &_vao);
                _num = 0;
            }
        }

        GLuint rawObject()
        {
            return _vao;
        }

        GLuint number() const
        {
            return _num;
        }

        void bind()
        {
            glBindVertexArray(_vao);
        }

        static void endBind()
        {
            glBindVertexArray(0);
        }

        template<class T> static void sEnableSliceAttribute()
        requires is_vertex_data_slice<T>::value
        {
            glEnableVertexAttribArray(T::index);
        }
        template<class T> static void sDisableSliceAttribute()
        requires is_vertex_data_slice<T>::value
        {
            glDisableVertexAttribArray(T::index);
        }
        static void sEnableSliceAttribute(GLuint index)
        {
            glEnableVertexAttribArray(index);
        }
        static void sDisableSliceAttribute(GLuint index)
        {
            glDisableVertexAttribArray(index);
        }
        template<class T> static void sEnableVertexDataAttribute()
        requires is_vertex_data<T>::value
        {
            []<std::size_t... I>(std::index_sequence<I...>)
            {
                (glEnableVertexAttribArray(I), ...);
            }(std::make_index_sequence<T::slice_number>());
        }
        template<class T> static void sDisableVertexDataAttribute()
        requires is_vertex_data<T>::value
        {
            []<std::size_t... I>(std::index_sequence<I...>)
            {
                (glDisableVertexAttribArray(I), ...);
            }(std::make_index_sequence<T::slice_number>());
        }

#ifdef __GLPP_SUPPORT_DSA
        // same as sEnableSliceAttribute but not static
        // need opengl >= 4.5
        template<class T> void enableSliceAttribute()
        requires is_vertex_data_slice<T>::value
        {
            glEnableVertexArrayAttrib(_vao, T::index);
        }
        // same as sDisableSliceAttribute but not static
        // need opengl >= 4.5
        template<class T> void disableSliceAttribute()
        requires is_vertex_data_slice<T>::value
        {
            glDisableVertexArrayAttrib(_vao, T::index);
        }
        // same as sEnableSliceAttribute but not static
        // need opengl >= 4.5
        void enableSliceAttribute(GLuint index)
        {
            glEnableVertexArrayAttrib(_vao, index);
        }
        // same as sDisableSliceAttribute but not static
        // need opengl >= 4.5
        void disableSliceAttribute(GLuint index)
        {
            glDisableVertexArrayAttrib(_vao, index);
        }
        // same as sEnableVertexDataAttribute not static
        // need opengl >= 4.5
        template<class T> static void enableVertexDataAttribute()
        requires is_vertex_data<T>::value
        {
            []<std::size_t... I>(std::index_sequence<I...>)
            {
                (glEnableVertexArrayAttrib(_vao, I), ...);
            }(std::make_index_sequence<T::slice_number>());
        }
        // same as sDisableVertexDataAttribute but not static
        // need opengl >= 4.5
        template<class T> static void disableVertexDataAttribute()
        requires is_vertex_data<T>::value
        {
            []<std::size_t... I>(std::index_sequence<I...>)
            {
                (glDisableVertexArrayAttrib(_vao, I), ...);
            }(std::make_index_sequence<T::slice_number>());
        }
#endif

    protected:
        GLsizei _num;
        GLuint  _vao;
    };
} // namespace lux::gapi::opengl
