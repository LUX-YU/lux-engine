#pragma once
#include <type_traits>
#include <string>
#include <functional>
#include <vector>
#include <lux/system/visibility_control.h>

namespace lux::cxx
{
    using SubProgramFunc = std::function<int (int, char*[])>;

    template<class T> concept class_has_void_entry = requires(T t){
        {t()} -> std::same_as<int>;
    };

    template<class T> concept class_has_argument_entry = requires(T t){
        {t(std::declval<int>(), std::declval<char*[]>())} -> std::same_as<int>;
    };

    template<class T> concept class_has_entry = class_has_void_entry<T> || class_has_argument_entry<T>;

    class SubProgramRegister
    {
    public:
        LUX_EXPORT static void registProgram(const std::string&, SubProgramFunc);

        LUX_EXPORT static bool hasSubProgram(const std::string&);

        LUX_EXPORT static std::vector<std::string> listSubProgram();

        // if not exist, return -255
        LUX_EXPORT static int invokeSubProgram(const std::string&, int argc, char* argv[]);
    };

    template<class T>
    struct ProgramClassEntryRegister
    {
        // enable after c++ 20
        template<class... Args> requires class_has_entry<T>
        ProgramClassEntryRegister(const std::string& name, Args&&... args)
        {
            auto regist_wrapper = [... args = std::forward<Args>(args)](int argc, char* argv[]){
                T t{args...};
                if constexpr( class_has_void_entry<T> )
                    return t();
                else
                    return t(argc, argv);
            };
            SubProgramRegister::registProgram(name, std::move(regist_wrapper));
        }
    };

    struct ProgramFuncEntryRegister
    {
        template<class Func>
        ProgramFuncEntryRegister(const std::string& name, Func&& func)
        requires std::is_convertible_v<Func, std::function<int (void)>>
        {
            SubProgramRegister::registProgram(
                name, [_func = std::forward<Func>(func)](int argc, char* argv[]){return _func();}
            );
        }

        template<class Func>
        ProgramFuncEntryRegister(const std::string& name, Func&& func)
        requires std::is_convertible_v<Func, std::function<int (int, char*[])>>
        {
            SubProgramRegister::registProgram(
                name, [_func = std::forward<Func>(func)](int argc, char* argv[]){return _func(argc, argv);}
            );
        }
    };

    /*
        example:
        class SampleClass
        {
        public:
            Test(int, double){}

            int operator()(){}
            or
            int operator()(int argc, char* argv[]){}
        };
        RegistClassSubprogramEntry(Test, "test_class" ,  1, 2.125)
    */
    #define RegistClassSubprogramEntry(type, title, ...)\
    static ::lux::cxx::ProgramClassEntryRegister<type> func_name ## _regist_helper(title, __VA_ARGS__);

    /*
        example:
        static int __main(int argc, char* argv){return 0;}
        RegistFuncSubprogramEntry(__main, "test_func")
    */
    #define RegistFuncSubprogramEntry(type, title)\
    static ::lux::cxx::ProgramFuncEntryRegister func_name ## _regist_helper(title, &type);
} // namespace lux::cxx

