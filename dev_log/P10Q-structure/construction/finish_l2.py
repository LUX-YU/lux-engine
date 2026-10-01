from pathlib import Path
import json

repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
work=Path(__file__).resolve().parent

def write(name,s):
 p=repo/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(s,newline='\n')

# CPU qualification uses the actual monitor and execution runtime. UI lifecycle remains in
# the unchanged original integration test, now configured after workbench.
s=(repo/'editor/tests/integration/tasks/monitor.cpp').read_text()
s=s.replace('#include <lux/engine/editor/tasks/TaskView.hpp>','#include <lux/engine/editor/tasks/TaskMonitor.hpp>')
s=s.replace('#include <lux/engine/editor/desktop/ViewHost.hpp>\n','').replace('#include <lux/engine/ui/Root.hpp>\n','')
s=s.replace('    auto root = take(ui::Root::create(messages.dispatcherRef(), {.docking = false}));\n','')
s=s.replace('    desktop::ViewHost host(*root);\n','')
a=s.index('    auto first = tasks::makeTaskView(');b=s.index('    bool completed{};',a)
s=s[:a]+'''    object::LuxObject second(messages.dispatcherRef());
    unsigned direct_notifications{};
    auto direct = take(object::LuxObject::connect(
        &monitor, &tasks::TaskMonitor::changed, &second,
        [&](std::uint64_t revision) noexcept {
            assert(revision == monitor.revision());
            ++direct_notifications;
        }, object::EDelivery::DIRECT
    ));
'''+s[b:]
s=s.replace('    assert(root->update({{640, 480}, .016F}, nullptr));\n','')
s=s.replace('    assert(a->tasks().rows().data() == b->tasks().rows().data());\n',
'''    assert(direct_notifications == 1);
    const auto consumer_a = monitor.snapshot();
    const auto consumer_b = monitor.snapshot();
    assert(consumer_a.get() == consumer_b.get());
''')
s=s.replace('    a->tasks().requestCancel(task.id());\n', '    assert(monitor.requestCancel(task.id()));\n')
s=s.replace('    assert(a->tasks().rejectedCancellations().empty());\n','')
s=s.replace('    assert(host.close(aid) && host.close(bid) && take(host.drain()).completed == 2);\n','')
s=s.replace('after views closed','after subscribers detached')
s=s.replace('    bool next_done{};', '    direct.disconnect();\n    bool next_done{};')
s=s.replace('XQ05 actual Runtime: two views share rows; FULL/CLOSED preserve terminal facts; last view does not ',
            'XL13 actual CPU Runtime: two subscribers share rows; FULL/CLOSED preserve terminal facts; detach does not ')
write('editor/activities/tasks/test/monitor.cpp',s)
p=repo/'editor/context/CMakeLists.txt';s=p.read_text()
s=s.replace('lux::engine::editor::tasks_ui','lux::engine::editor::editor_tasks')
s=s.replace('lux-engine-editor-tasks-ui REQUIRED COMPONENTS tasks_ui','lux-engine-editor-tasks REQUIRED COMPONENTS editor_tasks')
p.write_text(s,newline='\n')

# Role-specific interface exposure, not a link to the old editing product.
rulesfile=repo/'editor/tests/architecture/rules.json';r=json.loads(rulesfile.read_text())
for policy in [r['tasks_ui']]:
 policy['direct']=[x for x in policy['direct'] if x!='process_execution']+['editor_tasks']
 policy['closure']['editor_tasks']={'path':'editor/activities/tasks'}
for p in r.values():
 if isinstance(p,dict) and isinstance(p.get('closure'),dict) and 'tasks_ui' in p['closure']:
  p['closure']['editor_tasks']={'path':'editor/activities/tasks'}
rulesfile.write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n',newline='\n')
print('CPU TaskMonitor consumer and original Context observer dependency completed.')
