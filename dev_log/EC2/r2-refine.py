from pathlib import Path
s = Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p = s / 'editor/activities/scene/test/configuration.cpp'
t = p.read_text()
a = t.index('        if (reopened)\n')
b = t.index('        assert(reopened',a)
t = t[:a] + t[b:]
t = t.replace('#include <iostream>', '#include <iostream>\n#include <algorithm>')
t = t.replace('    const auto id =', '''    const auto same_systems = [](auto left, auto right) {
        // SceneDescriptionBuilder canonically orders provider bindings by requirement.
        for (auto* values : {&left, &right})
            for (auto& row : *values)
                std::ranges::sort(row.providers, {}, &SceneProviderBinding::requirement);
        return left == right;
    };
    const auto id =''')
t = t.replace('reopened->systems == draft->systems', 'same_systems(reopened->systems, draft->systems)')
t = t.replace('again->systems == draft->systems', 'same_systems(again->systems, draft->systems)')
p.write_text(t,newline='\n')
