include_guard(GLOBAL)

option(LAPIS_WARNINGS_AS_ERRORS "Fail builds on compiler warnings" ON)
set(LAPIS_SANITIZER "none" CACHE STRING "Runtime instrumentation: none, address, thread")
set_property(CACHE LAPIS_SANITIZER PROPERTY STRINGS none address thread)

# Apply only to lapis targets, not future third-party dependencies.
add_library(lapis_project_options INTERFACE)
target_compile_options(lapis_project_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
    -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
    -fno-omit-frame-pointer)
if(LAPIS_WARNINGS_AS_ERRORS)
    target_compile_options(lapis_project_options INTERFACE -Werror)
endif()

if(NOT LAPIS_SANITIZER STREQUAL "none")
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "lapis sanitizer presets currently require Clang/GCC on macOS or Linux")
    endif()
    if(LAPIS_SANITIZER STREQUAL "address")
        set(lapis_sanitizer_flags -fsanitize=address,undefined -fno-sanitize-recover=all)
    elseif(LAPIS_SANITIZER STREQUAL "thread")
        set(lapis_sanitizer_flags -fsanitize=thread)
    else()
        message(FATAL_ERROR "Unknown LAPIS_SANITIZER: ${LAPIS_SANITIZER}")
    endif()
    target_compile_options(lapis_project_options INTERFACE ${lapis_sanitizer_flags})
    target_link_options(lapis_project_options INTERFACE ${lapis_sanitizer_flags})
endif()
