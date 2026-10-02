from pathlib import Path
import subprocess
s=Path(r"E:/SyncForder/CodeRepos/lux-engine-p12")
p=s/"cmake/installed-consumers/editor-d2/GuiGesture.cpp"
old=subprocess.check_output(["git","show","HEAD:cmake/installed-consumers/editor-d2/GuiGesture.cpp"],cwd=s,text=True)
f=old[old.index("    void checkCompletedGesture("):old.rindex("} // namespace consumer")]
a=f.index("    {",f.index("void checkCompletedGesture"))
f="    void checkCompletedGesture(author::InspectorFields& interaction, author::SceneSession& session)\n"+f[a:]
f=f.replace('        ui::InspectorInteraction interaction(editing, "completed-gesture");', '        assert(interaction.refresh());\n        const auto object = interaction.target();')
f=f.replace('history.view()->snapshot','session.historyView()->snapshot').replace('history.undo()', 'session.undo()').replace('editing.component(', 'interaction.component(')
f=f.replace('            ImGui::Render();', '            ImGui::Render();\n            assert(interaction.update());')
f=f.replace('        assert(session.undo() && read() == original);','        assert(session.undo());\n        assert(interaction.refresh());\n        assert(read() == original);')
x=p.read_text()
x=x.replace('#include <source_location>', '#include <source_location>\n#include <imgui.h>\n#include <lux/engine/ui/Table.hpp>\n#include <InspectorControl.hpp>')
x=x.replace('}\nvoid consumer::checkInspector()', f+'}\nvoid consumer::checkInspector()')
x=x.replace('    assert(host.close(id) && host.drain());', '    {\n        author::InspectorFields fields(store.access<author::SceneSession>(), interaction, target, schema);\n        checkCompletedGesture(fields, *session);\n    }\n    assert(host.close(id) && host.drain());',1)
x=x.replace('    const auto countObjects =', '    const auto flags_before = component().flags;\n    assert(setField("flags", std::vector<bool>{false, true, false}));\n    assert(component().flags == (std::vector<bool>{false, true, false}));\n    assert(session->undo() && component().flags == flags_before);\n    frame();\n    const auto countObjects =',1)
p.write_text(x)
