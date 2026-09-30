from pathlib import Path
root=Path('E:/SyncForder/CodeRepos/lux-engine')
for folder, kind, queue, method in [('material','Material','canvas_request_','applyEdits'),('flowforge','Flow','canvas_edit_','apply')]:
    p=root/f'editor/tools/{folder}/ui/src/{kind}View.cpp'
    s=p.read_text()
    start=s.index(f'        {kind}ViewResult<void> {method}(std::vector')
    end=s.index(f'        {kind}ViewResult<void> maintain()', start)
    s=s[:start]+s[end:]
    start=s.index('            if (!edits_.empty())', s.index('while (!'+queue))
    end=s.index('            const auto command' if kind=='Material' else '            FlowViewResult<void> controlled;',start)
    s=s[:start]+s[end:]
    marker='            while (!'+queue+'.empty())'
    intro=f'''            if (!edits_.empty())
            {{
                if ({queue}.size() >= 64) return rejected(views::EViewError::CAPACITY);
                auto owner = services_.sessions.read(binding_->session);
                if (!owner) return rejected({kind}EditError{{owner.error()}});
                if (owner->get().describe().current != display_.content)
                    return rejected({kind}EditError{{E{kind}EditError::STALE_CONTENT}});
                auto read = owner->get().read();
                if (!read) return rejected(read.error());
                auto queued = read->withRead([&]() -> {kind}EditResult<void> {{
                    // The same bounded delivery stages serve property buttons and graph gestures.
                    // A BUSY preview/commit resumes that stage; it never starts a second gesture.
                    {queue}.push_back({{widgets::CanvasEdit{{{{}}, true, true, false}}, std::move(edits_), ECanvasStage::BEGIN}});
                    edits_.clear();
                    return {{}};
                }});
                if (!queued) return rejected(queued.error());
            }}
'''
    s=s.replace(marker,intro+marker)
    s=s.replace(f'                {queue}.pop_front();',f'''                auto owner = services_.sessions.read(binding_->session);
                if (!owner) return rejected({kind}EditError{{owner.error()}});
                auto read = owner->get().read();
                if (!read) return rejected(read.error());
                auto cleared = read->withRead([&]() -> {kind}EditResult<void> {{
                    {queue}.pop_front(); // Replaced preview payloads die under the author gate.
                    return {{}};
                }});
                if (!cleared) return rejected(cleared.error());''')
    if kind=='Material':
        s=s.replace('''                    std::vector<VMaterialEdit> edit;
                    edit.emplace_back(std::move(*draft_));
                    draft_.reset();
                    command_result = applyEdits(edit, true, true);''','''                    auto owner = services_.sessions.read(binding_->session);
                    if (!owner) return rejected(MaterialEditError{owner.error()});
                    auto read = owner->get().read();
                    if (!read) return rejected(read.error());
                    command_result = accepted(read->withRead([&]() -> MaterialEditResult<void> {
                        edits_.emplace_back(std::move(*draft_));
                        draft_.reset();
                        return {};
                    }));''')
        s=s.replace('const auto command = std::exchange(control_, EControl::NONE);', 'const auto command = control_;')
        s=s.replace('''            if (!command_result)
                return command_result;''','''            if (command_result || !temporary(command_result.error())) control_ = EControl::NONE;
            if (!command_result) return command_result;''')
    else:
        s=s.replace('switch (std::exchange(control_, EControl::NONE))','switch (control_)')
        s=s.replace('''            if (!controlled)
                return controlled;''','''            if (controlled || !temporary(controlled.error())) control_ = EControl::NONE;
            if (!controlled) return controlled;''')
    # Intent failures are classified precisely; no recursive retry of accepted operations.
    marker='        struct Display final'
    helper=f'''        bool temporary(const V{kind}ViewFailure& failure)
        {{
            return std::visit([](const auto& error) {{
                using T = std::decay_t<decltype(error)>;
                if constexpr (std::same_as<T, {kind}EditError>)
                    return error.code == E{kind}EditError::SESSION && error.session == sessions::ESessionError::BUSY;
                else if constexpr (std::same_as<T, views::EViewError>) return error == views::EViewError::BUSY;
'''
    if kind=='Material': helper+='''                else if constexpr (std::same_as<T, VMaterialCompileFailure>)
                {
                    const auto* code = std::get_if<EMaterialCompileRequestError>(&error);
                    return code && *code == EMaterialCompileRequestError::BUSY;
                }
'''
    else: helper+='''                else if constexpr (std::same_as<T, VFlowCompilationFailure>)
                {
                    const auto* code = std::get_if<EFlowCompileRequestError>(&error);
                    return code && *code == EFlowCompileRequestError::BUSY;
                }
'''
    helper+='''                else return false;
            }, failure);
        }
'''
    s=s.replace(marker,helper+marker)
    p.write_text(s)
