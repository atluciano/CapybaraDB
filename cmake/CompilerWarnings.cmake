include_guard(GLOBAL)

function(capybaradb_set_compiler_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /WX
            /utf-8
            /permissive-
            /Zc:__cplusplus
            /EHsc
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
        )
    endif()
endfunction()

function(capybaradb_enable_sanitizers_if_requested target)
    if(CAPYBARADB_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined
        )
    endif()
endfunction()
