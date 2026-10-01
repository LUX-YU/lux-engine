#include "allocations.hpp"
#include "SceneFixture.hpp"
#include <lux/engine/editor/flowforge/FlowInteraction.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/editor/scene/OutlinerView.hpp>
#include <lux/engine/ui/Root.hpp>
#include <algorithm>

template<class F> void profile(const char* label, std::size_t n, std::size_t selected, F f)
{
    std::vector<double> samples;
    std::size_t calls{}, bytes{};
    for(int i{};i!=110;++i)
    {
        allocation_probe::begin();
        auto start=std::chrono::steady_clock::now(); f();
        auto end=std::chrono::steady_clock::now(); allocation_probe::enabled=false;
        if(i>=10)
        {
            samples.push_back(std::chrono::duration<double,std::micro>(end-start).count());
            calls+=allocation_probe::calls; bytes+=allocation_probe::bytes;
        }
    }
    for(std::size_t i{};i<samples.size();++i) std::fprintf(stderr,"%s n=%zu selected=%zu sample=%zu duration_us=%.3f\n",label,n,selected,i,samples[i]);
    std::ranges::sort(samples);
    std::printf("%s n=%zu selected=%zu warmup=10 samples=100 p50_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f new_calls_per_sample=%zu requested_bytes_per_sample=%zu (owner executable/static C++ new only)\n",
        label,n,selected,samples[50],samples[95],samples[99],samples.back(),calls/100,bytes/100);
    std::fflush(stdout);
}
void flowProfile(std::size_t count)
{
    namespace f=lux::editor::flowforge;
    namespace graph=lux::flowforge;
    sessions::SessionStore store{1};
    auto reservation=take(store.reserve<f::FlowSession>({"lux.editor.flowforge"}, contracts::CodeLease::builtin()));
    f::FlowAuthoringSource source{asset::AssetId{uuid("flow-profile")},"profile",{}};
    auto event=source.graph.addNodes(std::make_unique<graph::OnEventNode>("tick"));
    assert(source.graph.addExport({graph::FlowForgeExportNodeId{1},source.graph.getNode(event).node->id(),1234}));
    for(std::size_t i=1;i<count;++i) (void)source.graph.addNodes(std::make_unique<graph::BranchNode>());
    auto model=take(f::FlowSession::create(reservation.id(),sessions::BoundSource{source.id,"profile.flow"},std::move(source)));
    auto* author=model.get(); assert(store.prepare(reservation,model));
    auto key=take(store.key<f::FlowSession>(take(store.publish(reservation))));
    f::FlowInteraction interaction(store.access<f::FlowSession>(),key);
    auto captured=take(author->capture());
    for(std::size_t selected : {1u,16u,64u})
    {
        std::vector<graph::NodeId> ids;
        for(std::size_t i=count-selected;i<count;++i) ids.push_back(captured.source().nodes[i].id);
        assert(interaction.select(ids));
        profile("FlowInteraction synchronize",count,selected,[&]{assert(interaction.synchronize());});
    }
}
void sceneProfile(std::size_t count)
{
    Fixture f;
    SceneEditBatch batch{f.author->describe().current,"profile rows",{}};
    for(std::size_t i=1;i<count;++i)
        batch.edits.push_back(SceneCreateObject{{world::WorldObjectId{uuid("row"+std::to_string(i))},{0},{}}});
    assert(f.author->apply(std::move(batch)));
    profile("SceneSession frozen capture",count,0,[&]{auto snapshot=take(f.author->capture());});
    auto messages=take(object::ObjectMessageQueue::create(64));
    auto root=take(lux::ui::Root::create(messages.dispatcherRef(),{.docking=false}));
    auto key=take(f.authors.key<SceneSession>(f.author_id));
    SceneInteractionGroup interaction(f.authors.access<SceneSession>(),key,{1});
    OutlinerView tree(messages.dispatcherRef(),lux::ui::PaneId{"profile"},f.authors.access<SceneSession>(),EditedSceneBinding{key,&interaction},{},f.schemas);
    auto mount=take(root->prepareMount(tree));assert(root->commit(mount));
    assert(root->update({{800,600},.016F},nullptr));
    profile("Outliner stable Root update",count,0,[&]{assert(root->update({{800,600},.016F},nullptr));});
    profile("Outliner explicit rebind rows",count,0,[&]{assert(tree.rebind(EditedSceneBinding{key,&interaction}));});
    auto detach=take(root->prepareDetach(tree));assert(root->commit(detach));
    auto projection=take(f.hub.acquire(*f.author,f.environment()));f.frame();
    profile("SceneProjection stable update",count,0,[&]{assert(projection->update(*f.author));});
    double x=20;
    profile("SceneProjection changed field plus frozen rebuild and retirement",count,0,[&]{
        f.set(x++);
        auto adopted=projection->update(*f.author);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(!adopted && std::get_if<EProjectionError>(&adopted.error().cause)
              && std::get<EProjectionError>(adopted.error().cause)==EProjectionError::BUSY)
        {
            assert(std::chrono::steady_clock::now()<deadline);
            f.frame();std::this_thread::yield();adopted=projection->update(*f.author);
        }
        assert(adopted);
        for(int turn{};turn!=4;++turn) f.frame();
    });
    projection.reset();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(f.hub.size()) { assert(std::chrono::steady_clock::now()<deadline);f.frame();std::this_thread::yield(); }
    assert(f.hub.size()==0);
}
int main()
{
    for(std::size_t n:{100u,1000u}) { flowProfile(n); sceneProfile(n); }
}
