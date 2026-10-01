#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/tasks/TaskView.hpp>
#include <lux/engine/editor/material/PublishCompiledMaterial.hpp>
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/editor/persistence/WriteCoordinator.hpp>
#include "allocations.hpp"
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/ui/Root.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cassert>
#include <thread>
using namespace lux;
using namespace lux::editor;
namespace em = lux::editor::material;
template <class T> auto take(T value) { assert(value); return std::move(*value); }
template <class F> void measure(const char* label, std::size_t entries, std::size_t views, F f)
{
    std::vector<double> samples;
    for (int run{}; run != 110; ++run)
    {
        const auto begin = std::chrono::steady_clock::now();
        f();
        const auto end = std::chrono::steady_clock::now();
        if (run >= 10) samples.push_back(std::chrono::duration<double,std::micro>(end-begin).count());
    }
    for(std::size_t i{}; i<samples.size(); ++i) std::fprintf(stderr,"sample=%zu duration_us=%.3f\n",i,samples[i]);
    std::ranges::sort(samples);
    std::printf("%s entries=%zu views=%zu warmup=10 samples=100 p50_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f\n",
        label,entries,views,samples[50],samples[95],samples[99],samples.back());
    std::fflush(stdout);
}
void catalog(std::size_t count, std::size_t nviews)
{
    auto messages = take(object::ObjectMessageQueue::create(64));
    auto root = take(ui::Root::create(messages.dispatcherRef(), {.docking=false}));
    std::vector<AssetCatalogEntry> rows;
    const auto space = *uuids::uuid::from_string("abcdef12-3456-7890-abcd-ef1234567890");
    for (std::size_t i{}; i != count; ++i)
        rows.push_back({asset::AssetId{uuids::uuid_name_generator(space)(std::to_string(i))}, {}, 1,
            "package/asset-long-path-" + std::to_string(i)});
#ifdef QUALITY_AFTER
    project::ProjectCatalogModel source(messages.dispatcherRef(), 1);
    assert(source.replace("measure", std::move(rows)));
#else
    struct Source { project::ProjectCatalog catalog; std::size_t reads{}; } source{{{1,1}, "measure", std::move(rows)}};
    project::ProjectCatalogAccess access{&source,
        +[](const void* p)->project::ProjectQueryResult<project::ProjectCatalogVersion>{return static_cast<const Source*>(p)->catalog.version;},
        +[](const void* p)->project::ProjectQueryResult<project::ProjectCatalog>{
            auto& s=*const_cast<Source*>(static_cast<const Source*>(p));++s.reads;return s.catalog;},
        +[](const void*,AssetReference,std::uint32_t)->project::ProjectQueryResult<asset::AssetId>{return asset::AssetId{};}};
#endif
    std::vector<std::unique_ptr<project::ProjectView>> views;
    for (std::size_t i{};i!=nviews;++i)
    {
#ifdef QUALITY_AFTER
        auto view=std::make_unique<project::ProjectView>(messages.dispatcherRef(),ui::PaneId{"view"+std::to_string(i)},source);
#else
        auto view=std::make_unique<project::ProjectView>(messages.dispatcherRef(),ui::PaneId{"view"+std::to_string(i)},access,project::AssetOpenRequests{});
#endif
        auto mount=take(root->prepareMount(*view)); assert(root->commit(mount));views.push_back(std::move(view));
    }
    const auto initial = views.front()->catalog().assets.data();
    std::size_t shared{};
    for(auto& view:views) shared += view->catalog().assets.data()==initial;
    measure("BQ2 catalog: 10 stable Root updates per sample",count,nviews,[&]{
        for(int step{};step!=10;++step) assert(root->update({{800,600},.016F},nullptr));});
#ifdef QUALITY_AFTER
    assert(shared==nviews);
    std::printf("catalog shared_buffers=%zu distinct_buffers=1 stable_first_buffer=%d\n",shared,views.front()->catalog().assets.data()==initial);
#else
    assert(source.reads==nviews);
    std::printf("catalog read_calls=%zu copied_rows=%zu initial_distinct_buffers=%zu stable_read_calls=0\n",source.reads,source.reads*count,nviews);
#endif
    allocation_probe::begin();
    for(int step{};step!=1000;++step) assert(root->update({{800,600},.016F},nullptr));
    allocation_probe::end();
    for(auto& view:views){auto detach=take(root->prepareDetach(*view));assert(root->commit(detach));}
}
void taskViews(std::size_t count,std::size_t nviews,bool revision)
{
    auto execution=take(process::ExecutionRuntime::create({.cpu_concurrency=1,.cpu_queue_capacity=32,
        .task_capacity=count+1,.timer={count+1},.task_history_capacity=count+1}));
    auto messages=take(object::ObjectMessageQueue::create(64));
    auto root=take(ui::Root::create(messages.dispatcherRef(),{.docking=false}));
#ifdef QUALITY_AFTER
    tasks::TaskMonitor monitor(messages.dispatcherRef(),execution);
#else
    tasks::TaskQueryPort port = revision ? tasks::TaskQueryPort{execution,nullptr,+[](const void*) noexcept {return std::uint64_t{1};}} : tasks::TaskQueryPort{execution};
#endif
    std::vector<process::Task> work;work.reserve(count);
    std::size_t completed{};
    for(std::size_t i{};i!=count;++i)
        work.push_back(take(execution.submit({"task", "BQ2"},[timer=execution.timer()](process::TaskReporter) noexcept {
            return stdexec::upon_error(stdexec::then(timer.after(std::chrono::hours(1)),
                []() noexcept ->cxx::expected<int,process::ETimerError>{return 0;}),
                [](process::ETimerError e) noexcept ->cxx::expected<int,process::ETimerError>{return cxx::unexpected(e);});
        },[&](auto&& value) noexcept {assert(!value);++completed;})));
    take(execution.dispatchTaskEvents());
#ifdef QUALITY_AFTER
    assert(monitor.dispatchChanges().complete());
#endif
    std::vector<std::unique_ptr<tasks::TaskView>> views;
    for(std::size_t i{};i!=nviews;++i)
    {
#ifdef QUALITY_AFTER
        auto view=std::make_unique<tasks::TaskView>(messages.dispatcherRef(),ui::PaneId{"view"+std::to_string(i)},monitor);
#else
        auto view=std::make_unique<tasks::TaskView>(messages.dispatcherRef(),ui::PaneId{"view"+std::to_string(i)},port);
#endif
        auto mount=take(root->prepareMount(*view));assert(root->commit(mount));views.push_back(std::move(view));
    }
    assert(root->update({{800,600},.016F},nullptr));
    std::size_t changed_buffers{};
    measure(revision?"BQ2 tasks revision: 10 Root updates/sample":"BQ2 tasks default: 10 Root updates/sample",count,nviews,[&]{
        for(int step{};step!=10;++step){const auto* old=views.front()->tasks().rows().data();
            assert(root->update({{800,600},.016F},nullptr)); changed_buffers += views.front()->tasks().rows().data()!=old;}
    });
    std::size_t shared{};for(auto& view:views){assert(view->tasks().rows().size()==count);shared+=view->tasks().rows().data()==views.front()->tasks().rows().data();}
    std::printf("tasks shared_buffers=%zu first_view_buffer_changes=%zu (address-change count; not all allocator calls)\n",shared,changed_buffers);
#ifdef QUALITY_AFTER
    assert(shared==nviews && changed_buffers==0);
#endif
    allocation_probe::begin();
    for(int step{};step!=1000;++step) assert(root->update({{800,600},.016F},nullptr));
    allocation_probe::end();
    for(auto& view:views){auto detach=take(root->prepareDetach(*view));assert(root->commit(detach));}
    for(auto& task:work) assert(execution.requestStop(task.id()));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
    while(completed!=count){assert(std::chrono::steady_clock::now()<deadline);take(execution.collectCompletions());take(execution.dispatchTaskEvents());std::this_thread::yield();}
}
std::shared_ptr<const em::CompiledMaterial> compile(process::ExecutionRuntime& execution)
{
    sessions::SessionStore authors{1};
    auto slot=take(authors.reserve<em::MaterialSession>({"lux.editor.material"},contracts::CodeLease::builtin()));
    asset::AssetId id{*uuids::uuid::from_string("abcdef12-3456-7890-abcd-ef1234567890")};
    lux::material::MaterialSource source{id,"BQ4",{}};
    auto constant=std::make_unique<lux::material::ConstantNode>();constant->setType(lux::material::EValueType::VEC3);
    auto node=source.graph.addNode(std::move(constant));auto output=source.graph.addNode(std::make_unique<lux::material::OutputSurfaceNode>());
    assert(source.graph.connect(node,0,output,0));
    auto model=take(em::MaterialSession::create(slot.id(),sessions::BoundSource{id,"test"},std::move(source)));
    auto* author=model.get();assert(authors.prepare(slot,model)&&authors.publish(slot));
    auto op=take(em::MaterialCompileOperation::start(execution,take(author->capture())));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
    while(!op->ready()){assert(std::chrono::steady_clock::now()<deadline);take(execution.collectCompletions());take(execution.dispatchTaskEvents());std::this_thread::yield();}
    return take(op->result());
}
void bytes()
{
    using namespace persistence;
    auto runtime=take(process::ExecutionRuntime::create({.cpu_concurrency=1,.cpu_queue_capacity=32,.timer={16}}));
    const auto compiled=compile(runtime);
    for(std::size_t mib:{1,16,64})
    {
        auto owned=std::make_shared<const std::vector<std::byte>>(mib*1024*1024,std::byte{42});
        auto input=std::make_shared<em::CompiledMaterial>(*compiled);
        // Only transport is measured: the sized opaque payload is not presented as a valid material encoding.
        input->bytes=cxx::SharedBytes<>::fromOwner(owned,*owned);
        WriteCoordinator coordinator{{1,owned->size()}};
        std::size_t aliased{};
        measure("BQ4 actual Material publication admission (sized transport payload)",owned->size(),1,[&]{
#ifdef QUALITY_AFTER
            const auto ticket=take(em::publishCompiledMaterial(coordinator,{{"target"},"missing"},input));
#else
            const auto ticket=take(em::PublishCompiledMaterialOperation::start(coordinator,{{"target"},"missing"},input));
#endif
            auto ready=take(coordinator.takeReady());assert(ready);
            aliased += ready->artifact->bytes.data()==input->bytes.data();
            assert(coordinator.complete(ticket,NotPublished{{EPersistenceError::CANCELLED}}));
            assert(coordinator.acknowledge(ticket));
        });
        std::printf("payload_aliases=%zu/110 copied_payload_bytes_per_admission=%zu\n",aliased,aliased==110?0:owned->size());
        std::printf("observed peak input + ready distinct payload bytes=%zu (payload buffers only; not process RSS)\n",owned->size()*(aliased==110?1:2));
    }
}
int main(int argc,char** argv)
{
    assert(argc>=2);
    const std::string mode=argv[1];
    if(mode=="bytes")bytes();
    else {assert(argc==4);auto n=std::stoull(argv[2]),v=std::stoull(argv[3]);
        if(mode=="catalog")catalog(n,v);else taskViews(n,v,mode=="tasks-revision");}
}
