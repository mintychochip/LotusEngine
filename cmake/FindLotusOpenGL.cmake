# OpenGL for lotus: prefer CMake's FindOpenGL, else link libGL directly.
# Compilation uses glad; only the GL shared library is required at link time.

find_package(OpenGL QUIET)

if(OpenGL_FOUND AND TARGET OpenGL::GL)
    message(STATUS "Lotus: using OpenGL::GL from FindOpenGL")
    return()
endif()

set(_LOTUS_GL_CANDIDATES
    /lib64/libGL.so.1
    /usr/lib64/libGL.so.1
    /lib/libGL.so.1
    /usr/lib/libGL.so.1)
set(LOTUS_GL_LIBRARY "")
foreach(_cand IN LISTS _LOTUS_GL_CANDIDATES)
    if(EXISTS "${_cand}")
        set(LOTUS_GL_LIBRARY "${_cand}")
        break()
    endif()
endforeach()
if(NOT LOTUS_GL_LIBRARY)
    message(
        FATAL_ERROR
        "libGL.so.1 not found. Install mesa-libGL (runtime) or mesa-libGL-devel.")
endif()

add_library(OpenGL::GL UNKNOWN IMPORTED GLOBAL)
set_target_properties(OpenGL::GL PROPERTIES IMPORTED_LOCATION "${LOTUS_GL_LIBRARY}")

message(STATUS "Lotus: using imported libGL ${LOTUS_GL_LIBRARY}")
