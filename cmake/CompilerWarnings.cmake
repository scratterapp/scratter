function(am_enable_warnings target)
    if(MSVC)
        # C4324 ("structure was padded due to alignment specifier") fires on every
        # alignas(64) cache-line split, where the padding is the point.
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /GR- /EHs-c- /wd4324)
        # Tells the MSVC standard library that exceptions are off, otherwise its
        # headers warn (C4530) under /EHs-c- and /WX turns that into an error.
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
            -Wconversion
            -fno-exceptions
            -fno-rtti
        )
    endif()
endfunction()
