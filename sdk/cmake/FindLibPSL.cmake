# FindLibPSL.cmake (46e): find_package(LibPSL) for RitterOS's libpsl.so.5,
# a system library from upstream (CMake has no module of its own for it).
# sdk/ritteros.cmake puts this folder on CMAKE_MODULE_PATH after a
# project's own folders: a project's FindLibPSL.cmake (WebKit has one) wins.
# Sets LibPSL_FOUND, LibPSL_VERSION, LIBPSL_INCLUDE_DIR(S),
# LIBPSL_LIBRARY/LIBPSL_LIBRARIES and the target LibPSL::LibPSL.
find_path(LIBPSL_INCLUDE_DIR libpsl.h)
find_library(LIBPSL_LIBRARY NAMES psl)
if(LIBPSL_INCLUDE_DIR AND EXISTS "${LIBPSL_INCLUDE_DIR}/libpsl.h")
    file(STRINGS "${LIBPSL_INCLUDE_DIR}/libpsl.h" _psl_version REGEX "^#define PSL_VERSION \"")
    string(REGEX REPLACE "^#define PSL_VERSION \"([^\"]*)\".*" "\\1" LibPSL_VERSION "${_psl_version}")
endif()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibPSL REQUIRED_VARS LIBPSL_LIBRARY LIBPSL_INCLUDE_DIR
                                  VERSION_VAR LibPSL_VERSION)
if(LibPSL_FOUND)
    set(LIBPSL_INCLUDE_DIRS "${LIBPSL_INCLUDE_DIR}")
    set(LIBPSL_LIBRARIES "${LIBPSL_LIBRARY}")
    if(NOT TARGET LibPSL::LibPSL)
        add_library(LibPSL::LibPSL UNKNOWN IMPORTED)
        set_target_properties(LibPSL::LibPSL PROPERTIES IMPORTED_LOCATION "${LIBPSL_LIBRARY}"
                              INTERFACE_INCLUDE_DIRECTORIES "${LIBPSL_INCLUDE_DIR}")
    endif()
endif()
mark_as_advanced(LIBPSL_INCLUDE_DIR LIBPSL_LIBRARY)
