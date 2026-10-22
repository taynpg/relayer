# 统一编译告警级别。
#
# 原则：所有本仓库代码（core / filestore / app / server / cli / tests）都套用同一套
# 告警，三方库不套用（它们入仓时就没打算适配我们的告警集）。
#
# 用法：目标创建后调用 relayer_set_warnings(<target>)。

function(relayer_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
            /Zc:preprocessor
        )
        if(RELAYER_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Wunused
            -Woverloaded-virtual
            -Wconversion
            -Wsign-conversion
            -Wdouble-promotion
            -Wformat=2
        )
        if(RELAYER_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
