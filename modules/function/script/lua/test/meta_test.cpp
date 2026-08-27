#include <sol/state_view.hpp>
#include <test_target_file.hpp>
#include <meta_registration.hpp>
#include <lua_registration.hpp>
#include <iostream>

#include <lux/engine/function/script/lua/Lua.hpp>
#include <sol/sol.hpp>

double
func(float arg1, double arg2)
{
    return arg1 + arg2;
}

namespace ns1
{
    std::string func2(float arg1, double arg2)
    {
        return std::to_string(arg1 + arg2);
    }
}

static void
initSolLua(lux::script::lua::ScriptEngine* engine)
{
    sol::state_view lua(static_cast<lua_State*>(engine->state()));
    lua.open_libraries(sol::lib::base, sol::lib::math);
    LuxRegisterAllMetas_LUA(lua);
}

int
main()
{
    using namespace lux::meta;
    // Initialize meta module
    meta_module_init();
    auto& registry = ReflectionRegistry::instance();
    LuxRegisterAllMetas_META(registry);

    // Initialize Lua script engine
    auto* script_engine = new lux::script::lua::ScriptEngine();
    initSolLua(script_engine);

    auto test_class_meta = registry.findClass("TestClass");
    // print meta info
    for (const auto& field : test_class_meta->fields)
    {
        std::cout << "Field Name:" << field.name << std::endl;
        std::cout << "\tField Type:   " << field.type.name << std::endl;
        std::cout << "\tField Offset: " << field.offset << std::endl;
    }

    for (const auto& method : test_class_meta->methods)
    {
        std::cout << "Method Name:" << method.invokable.name << std::endl;
        std::cout << "\tMethod Type:   " << method.invokable.type_signature << std::endl;
        std::cout << "\tMethod RetType:   " << method.invokable.return_type.name << std::endl;
        std::cout << "\tMethod Args:   " << method.invokable.parameters.size() << std::endl;
    }

    // method call
    {
        std::cout << "-------------Method Call Test Start-------------" << std::endl;
        std::cout << "Create Object" << std::endl;
        void* mem = malloc(test_class_meta->type.size);
        test_class_meta->construct(mem);
        auto obj = reinterpret_cast<TestClass*>(mem);
        assert(obj);
        int arg1 = 10;
        std::string arg2 = "Hello";
        void* args[2]{&arg1, &arg2};
        int ret;

        test_class_meta->methods[0].invokable.invoker(obj, args, &ret);
        std::cout << "Method Call Result: " << ret << std::endl;
        test_class_meta->destruct(mem);
        free(mem);
        std::cout << "-------------Method Call Test End-------------" << std::endl;
    }

    {
        std::cout << "-------------Function Call Test Start-------------" << std::endl;
        // function call
        double func_ret;
        float arg1_f = 1.0f;
        double arg2_d = 2.4;
        void* args2[2]{&arg1_f, &arg2_d};
        auto func_meta = registry.findFunctionTyped<float, double>("func");
        func_meta->invokable.invoker(nullptr, args2, &func_ret);

        std::cout << "Function Call Result: " << func_ret << std::endl;
        std::cout << "-------------Function Call Test End-------------" << std::endl;
    }

    // Test lua
    const char* code =
        R"(
local lux = lux
assert(lux.func(1.5,2.5) == 4.0)
assert(lux.TestEnum.ENUM2 == 232)

local a = lux.TestClass.new()
assert(a:func(5,"hi") == 6)
)";
    {
        script_engine->setOnError([](std::string_view error) { std::cerr << "Lua Error: " << error << std::endl; });
        auto ref = script_engine->parseScript(code);
        if (!ref)
        {
            std::cerr << "parseScript failed";
            return 1;
        }

        (void)script_engine->runScript(*ref);
        std::cout << "[C++] all tests passed 🎉\n";
    }

    delete script_engine; // script_engine must destruct before meta_module_deinit
    meta_module_deinit();

    return 0;
}
