#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <cassert>
#include <chrono>
#include <cstdio>
using namespace lux::editor::commands;
int main() {
 std::printf("sizeof Descriptor=%zu Entry=%zu Handle=%zu\n",sizeof(CommandDescriptor),sizeof(CommandEntry),sizeof(CommandHandle));
 for (auto count : {35,256,1024}) {
  std::vector<std::shared_ptr<CommandEntry>> entries;
  for(int i=0;i<count;++i) entries.push_back(std::make_shared<CommandEntry>(lux::editor::contracts::CodeLease::builtin(),CommandDescriptor{CommandId{"test."+std::to_string(i)},"Label","Group","Ctrl+S"},[](const CommandQuery&)->CommandResult<CommandState>{return CommandState{true};},[](const CommandInvocation&)->CommandResult<DispatchReceipt>{return DispatchReceipt{ImmediateCompletion{}};}));
  const auto start=std::chrono::steady_clock::now(); auto snapshot=CommandRegistrySnapshot::create(std::move(entries),2048); assert(snapshot);
  const auto built=std::chrono::steady_clock::now(); const std::string name="test."+std::to_string(count-1);const CommandIdView id{name};
  for(int i=0;i<10000;++i) {auto h=snapshot->find(id);assert(h);}
  const auto end=std::chrono::steady_clock::now();
  std::printf("N=%d create_us=%.3f resolve_10000_us=%.3f source=installed_EC2_R1\n",count,std::chrono::duration<double,std::micro>(built-start).count(),std::chrono::duration<double,std::micro>(end-built).count());
 }
}
