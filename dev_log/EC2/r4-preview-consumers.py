from pathlib import Path
import re
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=s/'editor/tests/integration/material_activity/compilation.cpp';t=p.read_text().replace(', {}, 1, preview.target()',', {}, 1')
t=t.replace('    preview.setDesired(s12->key());', '    const auto s12_adoption = take(preview.setDesired(s12->key()));',1)
t=t.replace('    rename(*mat, "S12");','    const auto s10_adoption = take(preview.setDesired(s10->key()));\n    rename(*mat, "S12");',1)
t=t.replace('preview.receive(*s12, {})','preview.receive(s12_adoption, s12->result(), {})').replace('preview.receive(*s10, {})','preview.receive(s10_adoption, s10->result(), {})')
t=t.replace('preview.status().prepared == s12->key()','preview.status().prepared == s12_adoption')
t=t.replace('    preview.setDesired(stale_key);','    assert(preview.setDesired(stale_key));')
a=t.index('    auto mismatch = s12->key();');z=t.index('    assert(mat->describe().current',a)
t=t[:a]+'''    auto mismatch = s12_adoption;
    mismatch.target = s10_adoption.target;
    ++mismatch.generation;
    assert(preview.receive(mismatch, s12->result(), {}));
    assert(!preview.status().prepared);
    auto changed_settings = s12->key();
    ++changed_settings.settings.version;
    assert(preview.setDesired(changed_settings));
    assert(preview.receive(s12_adoption, s12->result(), {}));
    assert(!preview.status().prepared);
    em::MaterialPreview second{*runtime, {}};
    const auto first_target = take(preview.setDesired(s12->key()));
    const auto second_target = take(second.setDesired(s12->key()));
    assert(first_target.target != second_target.target && first_target.input == second_target.input);
    assert(preview.receive(first_target, compiled, {}) && second.receive(second_target, compiled, {}));
    assert(preview.status().prepared == first_target && second.status().prepared == second_target);
    assert(second.reset({}));
    const auto rebound = take(second.setDesired(s12->key()));
    assert(rebound.generation > second_target.generation);
    assert(second.receive(second_target, compiled, {}) && !second.status().prepared);
    assert(second.receive(rebound, compiled, {}) && second.status().prepared == rebound);
    (void)second.close();
    assert(!second.setDesired(s12->key()) && preview.status().prepared == first_target);
'''+t[z:]
t=t.replace('    preview.setDesired(bad->key());','    const auto bad_adoption = take(preview.setDesired(bad->key()));')
t=t.replace('assert(!bad->result() && !preview.receive(*bad, {}));','assert(!bad->result() && preview.receive(bad_adoption, bad->result(), {}));\n    assert(preview.status().compilation_failure);')
p.write_text(t)
p=s/'editor/tests/integration/material_activity/ownership.cpp';t=p.read_text()
for value in ['shared_result','retained','after_release']:
 t=re.sub(rf'{value}->(key|source|artifact|bytes)\b(?!\()',rf'{value}->\1()',t)
t=re.sub(r'value_result\.(key|source|artifact|bytes)\b(?!\()',r'value_result.\1()',t)
t=t.replace('result())->source->','result())->source()->');p.write_text(t)
p=s/'editor/tests/integration/scene_views/views.cpp';t=p.read_text().replace('preview.receive(*operation,','preview.receive(preview.status().desired, operation->result(),').replace('preview.status().accepted == operation->key()','preview.status().accepted->input == operation->key()');p.write_text(t)
print('R4 actual consumer special-member assertions retained; independent target and reset regressions added')
