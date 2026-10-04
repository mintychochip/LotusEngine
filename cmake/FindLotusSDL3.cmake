# Resolves SDL3 for lotus: prefer system SDL3-devel, else headers from
# dependencies/SDL + the installed libSDL3 shared library.

find_package(SDL3 3.4 QUIET CONFIG)

if(SDL3_FOUND)
    message(STATUS "Lotus: using system SDL3 (${SDL3_VERSION})")
    return()
endif()

set(_LOTUS_SDL3_INCLUDE "${CMAKE_SOURCE_DIR}/dependencies/SDL/include")
if(NOT EXISTS "${_LOTUS_SDL3_INCLUDE}/SDL3/SDL.h")
    message(
        FATAL_ERROR
        "SDL3 not found. Install SDL3-devel (sudo dnf install SDL3-devel) "
        "or initialize the dependencies/SDL submodule.")
endif()

set(_LOTUS_SDL3_CANDIDATES
    /lib64/libSDL3.so.0
    /usr/lib64/libSDL3.so.0
    /lib/libSDL3.so.0
    /usr/lib/libSDL3.so.0)
set(LOTUS_SDL3_LIBRARY "")
foreach(_cand IN LISTS _LOTUS_SDL3_CANDIDATES)
    if(EXISTS "${_cand}")
        set(LOTUS_SDL3_LIBRARY "${_cand}")
        break()
    endif()
endforeach()
if(NOT LOTUS_SDL3_LIBRARY)
    message(
        FATAL_ERROR
        "libSDL3.so.0 not found. Install SDL3 (runtime) or SDL3-devel.")
endif()

add_library(SDL3::SDL3 SHARED IMPORTED GLOBAL)
set_target_properties(
    SDL3::SDL3
    PROPERTIES IMPORTED_LOCATION "${LOTUS_SDL3_LIBRARY}"
               INTERFACE_INCLUDE_DIRECTORIES "${_LOTUS_SDL3_INCLUDE}")

message(
    STATUS
    "Lotus: using imported SDL3 ${LOTUS_SDL3_LIBRARY} "
    "+ vendored headers from dependencies/SDL")
