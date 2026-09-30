#include <lux/engine/editor/material/MaterialInteraction.hpp>
#include <cstdio>
#include <string_view>
using namespace lux::editor;
int main(int argc, char** argv) {
    if(argc!=2) return 64;
    const std::string_view mode=argv[1];
    sessions::AccessState access;
    material::MaterialSession author(access);
    material::MaterialInteraction interaction({author,access},{1});
    if(!interaction.select({1}) || !interaction.begin("uncommitted drag"))return 65;
    int released=0; bool released_outside_read=false;
    std::vector<material::VMaterialEdit> input;
    auto payload=std::shared_ptr<const void>(new int(42),[&](const void* p) noexcept {
        ++released; released_outside_read|=!access.reading; delete static_cast<const int*>(p);
    });
    input.push_back({50,std::move(payload)});
    if(!interaction.preview(input))return 66;
    const auto before=author.stamp;
    const auto scopes=access.read_scopes;
    if(mode=="busy-cancel" || mode=="busy-sync")access.rejection=sessions::ESessionError::BUSY;
    else if(mode=="stale-sync")access.rejection=sessions::ESessionError::STALE_SESSION;
    else if(mode!="live-cancel")return 67;
    auto result=mode.ends_with("sync")?interaction.synchronize():interaction.cancel();
    const bool reported_busy=!result && result.error()==sessions::ESessionError::BUSY;
    const bool overlay=interaction.overlay()!=nullptr;
    const auto selected=interaction.selection().size();
    bool contract;
    if(mode.starts_with("busy")) contract=reported_busy && overlay && selected==1 && released==0;
    else if(mode=="stale-sync")contract=bool(result) && !overlay && selected==0 && released==1;
    else contract=bool(result) && !overlay && selected==1 && released==1 && !released_outside_read;
    std::printf("mode=%s result_ok=%d reported_busy=%d author_alive=1 author_unchanged=%d overlay_retained=%d selection_count=%zu payload_released=%d cleanup_outside_read=%d read_scopes_added=%d contract_pass=%d\n",
      argv[1],bool(result),reported_busy,author.stamp==before,overlay,selected,released,released_outside_read,access.read_scopes-scopes,contract);
    // Reset the injected error before destroying the real production interaction object.
    access.rejection.reset();
    return contract?0:1;
}
