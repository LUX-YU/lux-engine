#include <lux/engine/toolchain/asset/model/ModelCooker.hpp>
#include <iostream>
#include <filesystem>
#include <array>
int main(int argc, char** argv) {
 if(argc!=2) return 2;
 const std::filesystem::path root=argv[1];
 const std::array<std::string,1> paths{"triangle.obj"};
 const auto plain=lux::toolchain::readModelSourceFiles(root,paths,4096);
 const auto extended=lux::toolchain::readModelSourceFiles(std::filesystem::path(L"\\\\?\\"+root.native()),paths,4096);
 const auto present=[](const auto& r){return r && r->size()==1 && r->front().state==lux::toolchain::EModelSourceState::PRESENT && r->front().bytes.size()==36;};
 std::cout<<"real installed readModelSourceFiles: path_length="<<(root/paths[0]).native().size()<<" ordinary_present="<<present(plain)<<" extended_present="<<present(extended)<<'\n';
 return present(plain) && present(extended) ? 0 : 1;
}
