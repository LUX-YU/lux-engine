#pragma once
// ISOLATION SHIM. This is not an installed Lux SDK header.
// Only the access-result, content-stamp and gate behaviours used by the original .cpp are modelled.
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <utility>
namespace lux::cxx { using std::unexpected; }
namespace lux::material {
    using NodeId = int;
    struct Graph {
        int value{7};
        const int* node(NodeId id) const { return id == 1 ? &value : nullptr; }
    };
    struct MaterialSource { Graph graph; };
}
namespace lux::editor::editing { using HistoryId = int; }
namespace lux::editor::sessions {
    enum class ESessionError { BUSY, STALE_SESSION, WRONG_THREAD, WRONG_STORE, WRONG_TYPE,
        NOT_PREPARED, STALE_CONTENT, INVALID_ARGUMENT };
    enum class EEditAdmission { AVAILABLE, READING };
    struct ContentStamp {
        struct State { editing::HistoryId history{9}; bool operator==(const State&) const = default; } state;
        int revision{1}; bool operator==(const ContentStamp&) const = default;
    };
    struct SessionInfo { ContentStamp current; EEditAdmission admission{EEditAdmission::AVAILABLE}; };
    template<class T> using SessionResult = std::expected<T, ESessionError>;
    template<class T> struct TSessionKey { int id{}; };
    // Forced rejection represents what the real Store returns while an unrelated slot is reclaiming.
    // The shim does not implement Lux allocation, slots, models, codecs, threads or callbacks.
    struct AccessState { std::optional<ESessionError> rejection; bool reading{}; int read_scopes{}; };
    template<class T> class TSessionAccess {
        T* target_; AccessState* state_;
    public:
        TSessionAccess(T& target, AccessState& state):target_(&target),state_(&state){}
        SessionResult<std::reference_wrapper<const T>> read(TSessionKey<T>) const {
            if(state_->rejection) return std::unexpected(*state_->rejection);
            return std::cref(*target_);
        }
        SessionResult<std::reference_wrapper<T>> edit(TSessionKey<T>) const {
            if(state_->rejection) return std::unexpected(*state_->rejection);
            return std::ref(*target_);
        }
        SessionResult<SessionInfo> describe(TSessionKey<T> key) const {
            auto found=read(key); if(!found) return std::unexpected(found.error());
            return found->get().describe();
        }
    };
}
namespace lux::editor::material {
    template<class T> using MaterialEditResult=std::expected<T,sessions::ESessionError>;
    struct VMaterialEdit { int position{}; std::shared_ptr<const void> payload; };
    struct MaterialEditBatch { sessions::ContentStamp expected; std::string label; std::vector<VMaterialEdit> edits; };
    struct MaterialEditReceipt { sessions::ContentStamp content; };
    class MaterialReadView {
        const lux::material::MaterialSource* source_; sessions::AccessState* state_;
    public:
        MaterialReadView(const lux::material::MaterialSource& src,sessions::AccessState& state):source_(&src),state_(&state){}
        template<class F> auto withRead(F&& f) const {
            using R=decltype(f(*source_));
            if(state_->reading) return R{std::unexpected(sessions::ESessionError::BUSY)};
            struct Scope { sessions::AccessState& s; Scope(sessions::AccessState& v):s(v){s.reading=true;++s.read_scopes;} ~Scope(){s.reading=false;} } guard(*state_);
            return f(*source_);
        }
    };
    class MaterialSession {
        sessions::AccessState* state_;
    public:
        lux::material::MaterialSource source;
        sessions::ContentStamp stamp;
        explicit MaterialSession(sessions::AccessState& s):state_(&s){}
        sessions::SessionInfo describe() const { return {stamp,state_->reading?sessions::EEditAdmission::READING:sessions::EEditAdmission::AVAILABLE}; }
        MaterialEditResult<MaterialReadView> read() const {
            if(state_->reading) return std::unexpected(sessions::ESessionError::BUSY);
            return MaterialReadView{source,*state_};
        }
        MaterialEditResult<MaterialEditReceipt> apply(MaterialEditBatch b) {
            if(state_->reading) return std::unexpected(sessions::ESessionError::BUSY);
            if(!b.edits.empty()) source.graph.value=b.edits.back().position;
            ++stamp.revision; return MaterialEditReceipt{stamp};
        }
    };
    // The actual class' relevant member layout and public call signatures are mirrored here.
    class MaterialInteraction final {
    public:
        MaterialInteraction(sessions::TSessionAccess<MaterialSession>,sessions::TSessionKey<MaterialSession>) noexcept;
        ~MaterialInteraction() noexcept;
        MaterialInteraction(const MaterialInteraction&)=delete;
        MaterialInteraction& operator=(const MaterialInteraction&)=delete;
        MaterialInteraction(MaterialInteraction&&)=delete;
        MaterialInteraction& operator=(MaterialInteraction&&)=delete;
        MaterialEditResult<void> begin(std::string);
        MaterialEditResult<void> preview(std::vector<VMaterialEdit>&);
        MaterialEditResult<MaterialEditReceipt> commit();
        MaterialEditResult<void> cancel();
        MaterialEditResult<void> synchronize();
        MaterialEditResult<void> select(std::vector<lux::material::NodeId>);
        const MaterialEditBatch* overlay() const noexcept {return gesture_?&*gesture_:nullptr;}
        std::span<const lux::material::NodeId> selection() const noexcept {return selection_;}
    private:
        sessions::TSessionAccess<MaterialSession> access_;
        sessions::TSessionKey<MaterialSession> key_;
        std::optional<MaterialEditBatch> gesture_;
        std::vector<lux::material::NodeId> selection_;
        editing::HistoryId selection_history_;
    };
}
