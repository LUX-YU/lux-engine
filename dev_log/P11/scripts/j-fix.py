from pathlib import Path
s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11')
p=s/'editor/application/extensions/src/Contributions.cpp';t=p.read_text().replace('appended.error().code','appended.error().error').replace('committed.error().code','committed.error().error');p.write_text(t)
p=s/'editor/activities/sessions/src/SessionInstallation.cpp';t=p.read_text();needle='    InstalledSession::InstalledSession';i=t.index(needle);t=t[:i]+'''    namespace
    {
        template<class Invoke> auto callHistory(const detail::SessionInstallationData& data,Invoke invoke)
            -> decltype(invoke(*data.history))
        {
            if (data.code.sameOwner(contracts::CodeLease::builtin())) return invoke(*data.history);
            try { return invoke(*data.history); }
            catch(const std::bad_alloc&) { std::terminate(); }
            catch(...) { return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CALLBACK,"plugin.history"}); }
        }
    }
'''+t[i:];t=t.replace('return pinned->history->query();','return callHistory(*pinned,[](const HistoryActions& history) { return history.query(); });').replace('return pinned->history->undo();','return callHistory(*pinned,[](HistoryActions& history) { return history.undo(); });').replace('return pinned->history->redo();','return callHistory(*pinned,[](HistoryActions& history) { return history.redo(); });');p.write_text(t)
p=s/'editor/tests/architecture/CMakeLists.txt';t=p.read_text();t+='''
add_test(NAME editor.extensions.boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_layering_boundaries.py"
        --source "${PROJECT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}" --cmake "${CMAKE_COMMAND}" --p11)
set_tests_properties(editor.extensions.boundaries PROPERTIES LABELS "editor;native;P11" TIMEOUT 180)
''';p.write_text(t)
