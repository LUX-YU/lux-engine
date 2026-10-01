// Reduced control-flow witnesses, NOT the Lux SDK and NOT complete production .cpp files.
// The relevant scope ordering and independent publication guards match the inspected P11.
// Fixed cases are illustrative controls, not proposed repository patches.
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace source_cleanup {
struct SaveService;
struct Facts { bool code_alive=true; bool cleanup_saw_busy=false;
               bool nested_save_accepted=false; int destroyed=0; };
struct Role {
  virtual ~Role() = default;
};
struct Input { std::shared_ptr<int> code; std::unique_ptr<Role> source; };
struct SaveService {
  bool dispatching=false;
  struct DispatchScope {
    bool& active; const bool previous;
    explicit DispatchScope(bool& v):active(v), previous(std::exchange(v,true)){}
    ~DispatchScope(){active=previous;}
  };
  bool canPrepareSource() const { return !dispatching; }
  bool requestSave() { return !dispatching; } // Only the relevant dispatch admission check.
  bool prepareSource(std::unique_ptr<Role> source, std::shared_ptr<int> code, bool fixed) {
    Input owned{std::move(code),std::move(source)};
    if (dispatching) return false;
    const DispatchScope dispatch{dispatching};
    if (fixed) {
      auto admitted=std::move(owned);
      return false; // Same rejection, but cleanup is enclosed by dispatch.
    }
    return false;   // P11 shape: the later guard dies before the outer Input.
  }
};
struct ObservingRole:Role {
  SaveService& service; Facts& facts;
  ObservingRole(SaveService& s,Facts& f):service(s),facts(f){}
  ~ObservingRole() override {
    assert(facts.code_alive);
    ++facts.destroyed;
    facts.cleanup_saw_busy=!service.canPrepareSource();
    facts.nested_save_accepted=service.requestSave();
  }
};
bool run(bool fixed) {
  SaveService s; Facts f;
  auto code=std::shared_ptr<int>(new int(1),[&](int* p){f.code_alive=false; delete p;});
  bool accepted=s.prepareSource(std::make_unique<ObservingRole>(s,f),std::move(code),fixed);
  std::cout<<"case=cleanup fixed="<<fixed<<" rejected="<<!accepted
           <<" cleanup_saw_busy="<<f.cleanup_saw_busy
           <<" nested_save_accepted="<<f.nested_save_accepted
           <<" destroyed="<<f.destroyed
           <<" normal_after="<<s.canPrepareSource()<<'\n';
  return !accepted && f.cleanup_saw_busy && !f.nested_save_accepted
      && f.destroyed==1 && !f.code_alive && s.canPrepareSource();
}
}
namespace catalogs {
struct Commands {
  bool calling=false, dispatching=false, compound=false;
  unsigned revision=1; std::string current="A";
  bool canPublish(bool fixed) const { return !calling && !dispatching && !(fixed && compound); }
  bool publish(std::string candidate,bool fixed) {
    if(!canPublish(fixed)) return false;
    current=std::move(candidate); ++revision; return true;
  }
};
struct Contributions {
  Commands& commands; bool active=false; unsigned revision=1;
  std::string snapshot_commands="A";
  struct Scope { bool& v; explicit Scope(bool& x):v(x){v=true;} ~Scope(){v=false;} };
  bool withSnapshot(const std::function<void()>& callback,bool fixed) {
    if(active) return false;
    Scope scope{active};
    // Corrected control only: represent a lower-owner reservation, not a new event system.
    const bool previous=commands.compound;
    if(fixed) commands.compound=true;
    struct Restore { bool& v; bool before; ~Restore(){v=before;} } restore{commands.compound,previous};
    callback();
    return true;
  }
  bool failedPreparation(const std::function<void()>& reflection,bool fixed) {
    if(active || !commands.canPublish(fixed)) return false;
    Scope scope{active};
    const bool previous=commands.compound;
    if(fixed) commands.compound=true;
    struct Restore { bool& v; bool before; ~Restore(){v=before;} } restore{commands.compound,previous};
    reflection();
    return false; // Candidate's later configuration/reflection validation rejects it.
  }
};
bool run(bool fixed,bool failed) {
  Commands c; Contributions all{c}; bool nested=false; bool entered=false;
  auto callback=[&]{entered=true; nested=c.publish("C",fixed);};
  bool success=failed?all.failedPreparation(callback,fixed):all.withSnapshot(callback,fixed);
  bool consistent=c.current==all.snapshot_commands;
  std::cout<<"case="<<(failed?"failed_candidate":"batch")<<" fixed="<<fixed
           <<" entered="<<entered<<" outer_succeeded="<<success
           <<" nested_publish_accepted="<<nested
           <<" command_revision="<<c.revision<<" contribution_revision="<<all.revision
           <<" consistent="<<consistent<<'\n';
  return entered && !nested && consistent && c.revision==1
      && all.revision==1 && (failed?!success:success);
}
}
int main(int argc,char**argv){
  if(argc!=3) return 2;
  const std::string scenario=argv[1];
  const bool fixed=std::string(argv[2])=="fixed";
  bool pass=false;
  if(scenario=="cleanup") pass=source_cleanup::run(fixed);
  else if(scenario=="batch") pass=catalogs::run(fixed,false);
  else if(scenario=="failed_candidate") pass=catalogs::run(fixed,true);
  else return 2;
  return pass?0:1;
}
