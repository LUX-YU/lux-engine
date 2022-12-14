#pragma once
#include <type_traits>
#include <string>
#include <functional>
#include <vector>

namespace lux::engine::platform
{
    using SubProgramFunc = std::function<int (int, char*[])>;

    class SubProgramRegister
    {
    public:
        static void registProgram(const std::string&, const SubProgramFunc&);

        static bool hasSubProgram(const std::string&);

        static std::vector<std::string> listSubProgram();

        // if not exist, return -255
        static int invokeSubProgram(const std::string&, int argc, char* argv[]);
    };

    template<class T>
    class SubProgramCRTP
    {
    public:
        int __main(int argc, char* argv[])
        {
            return static_cast<T*>(this)->__main(argc, argv);
        }
    };

    template<class T>
    class SubProgramClassRegistHelper
    {
    public:
        template<class... Args>
        SubProgramClassRegistHelper(const std::string& name, Args&&... args)
        {
            static_assert( std::is_convertible_v(T*, SubProgramCRTP<T>*), "Error Type of T" );

            auto regist_wrapper = 
            [args = std::make_tuple(std::forward<Args>(args) ...)](int argc, char* argv[])-> int{
                return std::apply(
                    [argc, argv](auto&&... args)  -> int {
                        T instance(std::forward<Args>(args)...);
                        return instance.__main(argc, argv);
                    },
                    std::move(args)
                )
            };
            
            SubProgramRegister::registProgram(
                name, 
                std::move(regist_wrapper);
            );
        }
    };

    class SubProgramFunctionRegistHelper
    {
    public:
        SubProgramFunctionRegistHelper(const std::string& name, const SubProgramFunc& func)
        {
            SubProgramRegister::registProgram(
                name, 
                func
            );
        }
    };


    /*
        sample:

        class Test : public lux::engine::platform::SubProgramCRTP<Test>
        {
        public:
            Test(int, double){}
            int __main(int argc, char* argv){return 0;}
        };
        RegistClassSubProgram(Test, "test_class" ,  1, 2.125)
    */

    #define RegistClassSubProgram(class_name, title, ...)\
    static ::lux::engine::platform::SubProgramClassRegistHelper<class_name> class_name ## _regist_helper(title, __VA_ARGS__);

    #define RegistFunctionSubProgram(func_name, title)\
    static ::lux::engine::platform::SubProgramFunctionRegistHelper func_name ## _regist_helper(title, &func_name);
} // namespace lux::engine::platform

