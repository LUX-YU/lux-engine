# =============================================================================
#  no_terminal_io_test.cmake — 把「渲染库不写终端」钉成一条门禁。
#
#  为什么需要门禁而不是靠自觉:这条规则的每一次破坏都长得人畜无害 —— 加一行
#  cerr 说明刚发现的一个问题,当场是有用的。问题在它的**累积效应**:渲染库替
#  上层决定了诊断出现在哪个终端,于是编辑器想把渲染诊断显示在自己的面板里就
#  永远差一步。收敛用了七批,再漂回去只需要一次 code review 的疏忽。
#
#  失败点现在有两个正门:有调用方等结果的走返回值(Expected<T> + RenderError),
#  没有调用方的走自发上报(RenderContext::reportError)。两条都不经过终端。
#
#  test/ 不在扫描范围内 —— 那些是可执行示例,打印就是它们的产出。
#
#  扫描范围不止 render:ecs 的渲染桥同样是库,它的 13 处 cerr 就是趁着门禁只
#  管 render 长回来的 —— 规则只覆盖半个仓库,漏的那半边迟早补齐另一半的量。
# =============================================================================

# 每个根一行:<根目录>|<相对路径显示前缀>|<严格度>
#   basic  — 只禁 std::cerr/cout(render/ecs 老根;裸 fprintf 长尾还在迁)
#   strict — 连裸 fprintf(stderr 也禁(已完成 lux::log 迁移的 engine 组件,
#            §7.1;防"迁干净的模块又长回来")。产品输出型的例外(如
#            --dump-graph 的转储)用行内注记豁免:// no_terminal_io: allow
set(_scan_roots "${RENDER_ROOT}|render|basic")
if(ECS_ROOT)
    list(APPEND _scan_roots "${ECS_ROOT}|ecs|basic")
endif()
if(ENGINE_STRICT_ROOTS)
    # `|`-separated on the command line: a `;` list does not survive the
    # add_test → CTestTestfile escaping round-trip (it arrives as a literal
    # `\;` that foreach refuses to split).
    string(REPLACE "|" ";" _engine_roots "${ENGINE_STRICT_ROOTS}")
    foreach(_er IN LISTS _engine_roots)
        get_filename_component(_er_name "${_er}" NAME)
        list(APPEND _scan_roots "${_er}|engine/${_er_name}|strict")
    endforeach()
endif()

set(_offenders "")

foreach(_root_entry IN LISTS _scan_roots)
    string(REPLACE "|" ";" _parts "${_root_entry}")
    list(GET _parts 0 _root)
    list(GET _parts 1 _label)
    list(GET _parts 2 _mode)

    if(_mode STREQUAL "strict")
        set(_regex "(std::(cerr|cout)[ \t]*<<|fprintf[ \t]*\\([ \t]*stderr)")
    else()
        set(_regex "std::(cerr|cout)[ \t]*<<")
    endif()

    # render 是 include/sinclude/pinclude 三级可见性;ecs 各子模块自带
    # include/src,所以直接整根递归扫,靠下面的 test/ 排除挡掉示例。
    file(GLOB_RECURSE _sources
         "${_root}/*.cpp" "${_root}/*.hpp" "${_root}/*.h" "${_root}/*.inl")
    foreach(_file IN LISTS _sources)
        # test/ 可执行示例、app/ 宿主 main(usage 打印是白名单产品输出,
        # §7.1)都不算库代码。
        if(_file MATCHES "/test/" OR _file MATCHES "/app/")
            continue()
        endif()
        file(STRINGS "${_file}" _hits REGEX "${_regex}")
        if(_hits)
            file(RELATIVE_PATH _rel "${_root}" "${_file}")
            foreach(_hit IN LISTS _hits)
                if(_hit MATCHES "no_terminal_io: allow")
                    continue()   # 显式豁免(产品输出,见注记处的理由)
                endif()
                string(STRIP "${_hit}" _hit)
                # 命中行里的分号会把 CMake 列表拆成两项 —— 一处违规报成两处、
                # 输出还断成两行。转义后再入列表,报数与显示才对得上。
                string(REPLACE ";" "\\;" _hit "${_hit}")
                list(APPEND _offenders "  ${_label}/${_rel}: ${_hit}")
            endforeach()
        endif()
    endforeach()
endforeach()

if(_offenders)
    list(LENGTH _offenders _count)
    string(REPLACE ";" "\n" _report "${_offenders}")
    message(FATAL_ERROR
        "库代码里出现了 ${_count} 处终端写入:\n${_report}\n\n"
        "失败点的正门,都不经过终端:\n"
        "  · 有调用方在等结果 → 返回 Expected<T>,失败用 renderFailure<err::…>()\n"
        "  · 没有调用方(每帧路径、驱动回调)→ 自发上报:\n"
        "      render 侧 RenderContext::reportError(...)\n"
        "      ecs 桥   diagnoseRenderBridge(...)(应用装 setRenderBridgeDiagnosticSink)\n"
        "  · 诊断文本 → lux::log::info/warn/error(category, fmt, ...)(§7.1;\n"
        "      宿主装 sink 决定落点,库层只声明等级+分类)\n"
        "错误类型在 core/RenderErrorList.hpp 登记;实参槽只装数字,\n"
        "名字一类的东西用可索引的句柄表达(见该文件的族头注释)。")
endif()

message(STATUS "no_terminal_io: render + ecs + engine strict 根零终端写入")
