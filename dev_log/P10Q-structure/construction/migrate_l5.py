from pathlib import Path

repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
work=Path(__file__).resolve().parent
(repo/'editor/tests/architecture/test_delivery_constraints.py').write_bytes((work/'test_delivery_constraints.py').read_bytes())
p=repo/'editor/workbench/sinclude/lux/engine/editor/workbench/InteractionDelivery.hpp'
s=p.read_text().replace('#include <cstdint>','#include <concepts>\n#include <functional>\n#include <lux/cxx/compile_time/expected.hpp>\n#include <cstdint>')
marker='    enum class EInputDeliveryStage'
concepts='''    template <class R>
    concept VoidDeliveryResult = std::default_initializable<R> && std::move_constructible<R> &&
        requires(R& result, const R& observed) {
            typename R::error_type;
            requires std::same_as<R, lux::cxx::expected<void, typename R::error_type>>;
            { static_cast<bool>(observed) } -> std::same_as<bool>;
            { result.error() } -> std::same_as<typename R::error_type&>;
        };

    template <class F, class R>
    concept DeliveryAction = std::invocable<F&> && std::same_as<std::invoke_result_t<F&>, R>;

'''
s=s.replace(marker,concepts+marker)
s=s.replace('    auto deliverInput(','''    requires std::invocable<Validate&> && VoidDeliveryResult<std::invoke_result_t<Validate&>> &&
        DeliveryAction<Cancel, std::invoke_result_t<Validate&>> &&
        DeliveryAction<Begin, std::invoke_result_t<Validate&>> &&
        DeliveryAction<Preview, std::invoke_result_t<Validate&>> &&
        DeliveryAction<Commit, std::invoke_result_t<Validate&>>
    auto deliverInput(''')
for call in ['validate','cancel','begin','preview','finish']:
    s=s.replace(call+'();','std::invoke('+call+');')
p.write_text(s,newline='\n')
p=repo/'editor/tests/architecture/CMakeLists.txt'
with p.open('a',newline='\n') as out:out.write('''
if(MSVC)
    add_test(NAME editor.layering.delivery_constraints
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_delivery_constraints.py"
            --build "${CMAKE_BINARY_DIR}")
    set_tests_properties(editor.layering.delivery_constraints PROPERTIES LABELS "editor;native;editor_layering" TIMEOUT 120)
endif()
''')
print('One real delivery algorithm constrained; operation owners and domain actions unchanged.')
