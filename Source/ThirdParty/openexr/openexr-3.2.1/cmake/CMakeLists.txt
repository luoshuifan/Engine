# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Contributors to the OpenEXR Project.

include(CheckIncludeFiles)
include(CheckSymbolExists)
include(CheckLibraryExists)
include(CheckStructHasMember)
include(CheckCXXSourceCompiles)
include("${CMAKE_CURRENT_LIST_DIR}/JoinPaths.cmake")

#################### Config headers and interface libs ######################

###################################################
####### OpenEXRConfig.h and OpenEXRConfigInternal.h

if (NOT CMAKE_CROSSCOMPILING AND EXISTS "/proc/self/exe")
  set(OPENEXR_IMF_HAVE_LINUX_PROCFS TRUE)
endif()

check_cxx_source_compiles(
"
#include <unistd.h>
int main()
{
    sysconf(_SC_NPROCESSORS_ONLN);
}
" OPENEXR_IMF_HAVE_SYSCONF_NPROCESSORS_ONLN
)    

check_cxx_source_compiles(
"
int main()
{
#if defined(__SSE2__)
    int n   = 0;
    int eax = 0;
    int edx = 0;
    __asm__(
        \"xgetbv     ;\"
        \"vzeroupper  \"
        : \"=a\"(eax), \"=d\"(edx) : \"c\"(n) : );
#else
#    error No SSE support enabled to query AVX support
#endif
}
" OPENEXR_IMF_HAVE_GCC_INLINE_ASM_AVX
)


# This is no longer something that needs to be tested if we have
# C++14 support...
set(OPENEXR_IMF_HAVE_COMPLETE_IOMANIP TRUE)

if(APPLE)
  set(OPENEXR_IMF_HAVE_DARWIN TRUE)
endif()

if (OPENEXR_ENABLE_LARGE_STACK)
  set(OPENEXR_HAVE_LARGE_STACK ON)
endif()
if (OPENEXR_USE_DEFAULT_VISIBILITY)
  set(OPENEXR_ENABLE_API_VISIBILITY OFF)
else()
  set(OPENEXR_ENABLE_API_VISIBILITY ON)
endif()

if (UNIX AND NOT APPLE AND NOT BEOS)
  find_library(OPENEXR_EXTRA_MATH_LIB m)
  mark_as_advanced(OPENEXR_EXTRA_MATH_LIB)
endif()

configure_file(OpenEXRConfig.h.in ${CMAKE_CURRENT_BINARY_DIR}/OpenEXRConfig.h)
configure_file(OpenEXRConfigInternal.h.in ${CMAKE_CURRENT_BINARY_DIR}/OpenEXRConfigInternal.h)
# make a temp copy in the binary dir for OpenEXRConfig.h to include
configure_file(../src/lib/OpenEXRCore/openexr_version.h ${CMAKE_CURRENT_BINARY_DIR}/OpenEXRCore/openexr_version.h COPYONLY)

if(OPENEXR_INSTALL)
install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/OpenEXRConfig.h
  DESTINATION
    ${CMAKE_INSTALL_INCLUDEDIR}/${OPENEXR_OUTPUT_SUBDIR}
)
endif()

###################################################
####### IexConfig.h and IexConfigInternal.h

check_include_files(ucontext.h IEX_HAVE_UCONTEXT_H)
if(IEX_HAVE_UCONTEXT_H)
  # TODO: remove this once we cleanly have IEX_ prefix on all #defines
  set(HAVE_UCONTEXT_H ON)
  check_struct_has_member("struct _libc_fpstate" mxcsr ucontext.h IEX_HAVE_CONTROL_REGISTER_SUPPORT)
  if(NOT IEX_HAVE_CONTROL_REGISTER_SUPPORT)
    check_include_files(asm/sigcontext.h IEX_HAVE_SIGCONTEXT_H)
    if(IEX_HAVE_SIGCONTEXT_H)
      check_struct_has_member("struct _fpstate" mxcsr asm/sigcontext.h IEX_HAVE_SIGCONTEXT_CONTROL_REGISTER_SUPPORT)
    endif()
  endif()
endif()

###check_include_files(pthread.h ILMTHREAD_HAVE_PTHREAD)
if(OPENEXR_ENABLE_THREADING AND Threads_FOUND)
  set(ILMTHREAD_THREADING_ENABLED ON)

  # we have threads, but do we have posix semaphores for sem_init?
  # should be in pthreads
  if(NOT (APPLE OR (WIN32 AND (NOT MINGW))))
    if(MINGW)
      check_include_files("pthread_unistd.h;semaphore.h" ILMTHREAD_HAVE_SEMAPHORE_H)
    else()
      check_include_files(semaphore.h ILMTHREAD_HAVE_SEMAPHORE_H)
    endif()
    if(ILMTHREAD_HAVE_SEMAPHORE_H)
      # should just be in libc, so no need for check_library_exists
      if(CMAKE_HAVE_LIBC_PTHREAD)
        set(ILMTHREAD_HAVE_POSIX_SEMAPHORES ON)
      else()
        set(CMAKE_REQUIRED_FLAGS ${CMAKE_THREAD_LIBS_INIT})
        check_library_exists(pthread sem_init "" ILMTHREAD_HAVE_POSIX_SEMAPHORES)
        set(CMAKE_REQUIRED_FLAGS)
      endif()
    endif()
  endif()
endif()

configure_file(IexConfig.h.in ${CMAKE_CURRENT_BINARY_DIR}/IexConfig.h)
configure_file(IexConfigInternal.h.in ${CMAKE_CURRENT_BINARY_DIR}/IexConfigInternal.h)
configure_file(IlmThreadConfig.h.in ${CMAKE_CURRENT_BINARY_DIR}/IlmThreadConfig.h)

###################################################
####### Install public config headers

if(OPENEXR_INSTALL)
install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/OpenEXRConfig.h
    ${CMAKE_CURRENT_BINARY_DIR}/IexConfig.h
    ${CMAKE_CURRENT_BINARY_DIR}/IlmThreadConfig.h
  DESTINATION
    ${CMAKE_INSTALL_INCLUDEDIR}/${OPENEXR_OUTPUT_SUBDIR}
)
endif()

###################################################
####### Interface libraries for configs

add_library(OpenEXRConfig INTERFACE)
target_include_directories(OpenEXRConfig INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/${OPENEXR_OUTPUT_SUBDIR}>)

if(OPENEXR_INSTALL)
  install(TARGETS OpenEXRConfig EXPORT ${PROJECT_NAME})
endif()
add_library(${PROJECT_NAME}::Config ALIAS OpenEXRConfig)

add_library(IexConfig INTERFACE)
target_include_directories(IexConfig INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/${OPENEXR_OUTPUT_SUBDIR}>)

if(OPENEXR_INSTALL)
  install(TARGETS IexConfig EXPORT ${PROJECT_NAME})
endif()
add_library(${PROJECT_NAME}::Config ALIAS IexConfig)

add_library(IlmThreadConfig INTERFACE)
target_include_directories(IlmThreadConfig INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/${OPENEXR_OUTPUT_SUBDIR}>)

if(OPENEXR_INSTALL)
  install(TARGETS IlmThreadConfig EXPORT ${PROJECT_NAME})
endif()
add_library(${PROJECT_NAME}::Config ALIAS IlmThreadConfig)

###################################################
####### Install pkg-config files if requested

if(OPENEXR_INSTALL_PKG_CONFIG)

  # Prepend prefix for includedir only if CMAKE_INSTALL_INCLUDEDIR is relative
  if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
      set(PKG_CONFIG_INSTALL_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
  else()
      set(PKG_CONFIG_INSTALL_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
  endif()

  # Prepend exec_prefix for libdir only if CMAKE_INSTALL_LIBDIR is relative
  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
      set(PKG_CONFIG_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
  else()
      set(PKG_CONFIG_INSTALL_LIBDIR "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
  endif()

  # use a helper function to avoid variable pollution, but pretty simple
  function(openexr_pkg_config_help pcinfile)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" uppercase_CMAKE_BUILD_TYPE)
    set(LIB_SUFFIX_DASH ${OPENEXR_LIB_SUFFIX}${CMAKE_${uppercase_CMAKE_BUILD_TYPE}_POSTFIX})
    if(OPENEXR_ENABLE_THREADING AND TARGET Threads::Threads)
      # hrm, can't use properties as they end up as generator expressions
      # which don't seem to evaluate
      if(THREADS_HAVE_PTHREAD_ARG OR CMAKE_HAVE_LIBC_PTHREAD)
        set(exr_pthread_cflags "-pthread")
      else()
        set(exr_pthread_cflags "")
      endif()
      set(exr_pthread_libs ${CMAKE_THREAD_LIBS_INIT})
    endif()
    string(REPLACE ".in" "" pcout ${pcinfile})
    configure_file(${pcinfile} ${CMAKE_CURRENT_BINARY_DIR}/${pcout} @ONLY)
    install(
        FILES ${CMAKE_CURRENT_BINARY_DIR}/${pcout}
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )
  endfunction()
  openexr_pkg_config_help(OpenEXR.pc.in)
  message(STATUS "OpenEXR pkg-config generation enabled")
else()
  message(STATUS "OpenEXR pkg-config generation disabled")
endif()

###################################################
####### Install cmake Config files

# The main export of the configuration - This is the
# moral equivalent of a pkg-config file for cmake
# and replaces the Find*.cmake of the "old" cmake

# The main export of the configuration - This is the
# moral equivalent of a pkg-config file for cmake
# and replaces the Find*.cmake of the "old" cmake
include(CMakePackageConfigHelpers)

if(OPENEXR_INSTALL)
configure_package_config_file(${PROJECT_NAME}Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)
write_basic_package_version_file("${PROJECT_NAME}ConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

install(EXPORT ${PROJECT_NAME}
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
  FILE ${PROJECT_NAME}Targets.cmake
  NAMESPACE ${PROJECT_NAME}::
  EXPORT_LINK_INTERFACE_LIBRARIES
)
endif()
