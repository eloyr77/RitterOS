# CMake's platform description of RitterOS (43d; sdk/ritteros.cmake names
# it): an ELF system with shared libraries (libNAME.so, a soname), the C
# library always linked. (43e) Programs look for libraries in $ORIGIN/lib
# first (sdk/ritteros.cmake's link: an app's own, in its bundle), then in
# /system/ARCH/lib; a project's own INSTALL_RPATH is written at the build
# (no build-tree paths: the files are copied as built).
set(UNIX 1)
set(RITTEROS 1)
set(CMAKE_DL_LIBS "")
set(CMAKE_EXECUTABLE_SUFFIX "")
set(CMAKE_SHARED_LIBRARY_PREFIX "lib")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".so")
set(CMAKE_SHARED_MODULE_PREFIX "lib")
set(CMAKE_SHARED_MODULE_SUFFIX ".so")
set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".a")
set(CMAKE_FIND_LIBRARY_PREFIXES "lib")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".so" ".so.1" ".a")
# (46e) The one prefix the find_* calls look under, joined to each of
# sdk/ritteros.cmake's CMAKE_FIND_ROOT_PATH: ROOT/lib and ROOT/include
# (Linux's UnixPaths lists /usr, /usr/local and others too).
set(CMAKE_SYSTEM_PREFIX_PATH "/")
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
set(CMAKE_SHARED_LIBRARY_SONAME_C_FLAG "-soname ")
set(CMAKE_SHARED_LIBRARY_SONAME_CXX_FLAG "-soname ")
set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG "-rpath ")
set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG_SEP ":")
set(CMAKE_SHARED_LIBRARY_RUNTIME_CXX_FLAG "-rpath ")
set(CMAKE_SHARED_LIBRARY_RUNTIME_CXX_FLAG_SEP ":")
set(CMAKE_SKIP_BUILD_RPATH TRUE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_C_COMPILE_OPTIONS_PIC "-fPIC")
set(CMAKE_C_COMPILE_OPTIONS_PIE "-fPIE")
set(CMAKE_CXX_COMPILE_OPTIONS_PIC "-fPIC")
set(CMAKE_CXX_COMPILE_OPTIONS_PIE "-fPIE")
set(CMAKE_SHARED_LIBRARY_C_FLAGS "-fPIC")
set(CMAKE_SHARED_LIBRARY_CXX_FLAGS "-fPIC")
set(CMAKE_INCLUDE_FLAG_C "-I")
set(CMAKE_INCLUDE_FLAG_CXX "-I")
set(CMAKE_LIBRARY_PATH_FLAG "-L")
set(CMAKE_LINK_LIBRARY_FLAG "-l")
