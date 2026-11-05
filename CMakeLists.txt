cmake_minimum_required(VERSION 3.28)

project(gsradio
  VERSION 0.1
  DESCRIPTION "IceS 0.4 compatible MP3 streaming source for Icecast, built on GStreamer"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

option(ICES_WITH_PYTHON "Embed Python for scripted playlists" ON)
option(ICES_WITH_PERL   "Embed Perl for scripted playlists"   ON)

set(ICES_ETCDIR    "${CMAKE_INSTALL_FULL_SYSCONFDIR}" CACHE PATH "Directory searched for ices.conf")
set(ICES_MODULEDIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/ices/modules" CACHE PATH "Playlist module directory")
# If set, the sample ices.conf/ices.py/ices.pm are installed here instead of into
# ICES_ETCDIR and ICES_MODULEDIR (packaging systems copy them into place).
set(ICES_EXAMPLESDIR "" CACHE STRING "Install sample configuration files here (relative to the prefix)")
# pkg-config module providing the embeddable Python library.
set(ICES_PYTHON_PKGCONFIG "python3-embed" CACHE STRING "pkg-config module for embedding Python")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0>=1.20 gstreamer-app-1.0 gstreamer-audio-1.0 gstreamer-tag-1.0)
pkg_check_modules(SHOUT  REQUIRED IMPORTED_TARGET shout>=2.4.3)
pkg_check_modules(LIBXML REQUIRED IMPORTED_TARGET libxml-2.0)

add_executable(ices
  src/main.cc
  src/config.cc
  src/log.cc
  src/signals.cc
  src/mp3.cc
  src/source.cc
  src/decoder.cc
  src/encoder.cc
  src/crossfade.cc
  src/shout_stream.cc
  src/metadata.cc
  src/cue.cc
  src/streamer.cc
  src/playlist.cc
  src/playlist_builtin.cc
)

target_link_libraries(ices PRIVATE PkgConfig::GST PkgConfig::SHOUT PkgConfig::LIBXML)
# Project headers are included by their path from the repository root.
target_include_directories(ices PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(ices PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Wno-missing-field-initializers>)

# ---- liblame (used to ask LAME for its default output sample rate) ---------
find_path(LAME_INCLUDE_DIR lame/lame.h)
find_library(LAME_LIBRARY NAMES mp3lame)
if(LAME_INCLUDE_DIR AND LAME_LIBRARY)
  target_include_directories(ices PRIVATE ${LAME_INCLUDE_DIR})
  target_link_libraries(ices PRIVATE ${LAME_LIBRARY})
  target_compile_definitions(ices PRIVATE ICES_HAVE_LIBLAME=1)
  message(STATUS "liblame: yes (${LAME_LIBRARY})")
else()
  message(STATUS "liblame: no (default output rates are approximated)")
endif()

# ---- Embedded Python -------------------------------------------------------
set(ICES_HAVE_PYTHON 0)
if(ICES_WITH_PYTHON)
  pkg_check_modules(PYEMBED IMPORTED_TARGET ${ICES_PYTHON_PKGCONFIG})
  if(PYEMBED_FOUND)
    set(ICES_HAVE_PYTHON 1)
    target_sources(ices PRIVATE src/playlist_python.cc)
    target_link_libraries(ices PRIVATE PkgConfig::PYEMBED)
    message(STATUS "Python playlist support: yes (${PYEMBED_VERSION})")
  else()
    message(STATUS "Python playlist support: no (${ICES_PYTHON_PKGCONFIG} not found)")
  endif()
endif()

# ---- Embedded Perl ---------------------------------------------------------
set(ICES_HAVE_PERL 0)
if(ICES_WITH_PERL)
  find_program(PERL_EXECUTABLE perl)
  if(PERL_EXECUTABLE)
    execute_process(COMMAND ${PERL_EXECUTABLE} -MExtUtils::Embed -e ccopts
                    OUTPUT_VARIABLE PERL_CCOPTS OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE PERL_CC_RC ERROR_QUIET)
    execute_process(COMMAND ${PERL_EXECUTABLE} -MConfig -e "print \$Config{archlibexp}, '/CORE'"
                    OUTPUT_VARIABLE PERL_CORE_DIR)
    execute_process(COMMAND ${PERL_EXECUTABLE} -MConfig -e "print \$Config{libperl}"
                    OUTPUT_VARIABLE PERL_LIBNAME)
    execute_process(COMMAND ${PERL_EXECUTABLE} -MConfig -e "print \$Config{perllibs}"
                    OUTPUT_VARIABLE PERL_SYSLIBS)
    find_library(PERL_LIBRARY NAMES perl ${PERL_LIBNAME}
                 HINTS ${PERL_CORE_DIR} /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE} /usr/lib /usr/local/lib)
    if(PERL_CC_RC EQUAL 0 AND PERL_LIBRARY AND EXISTS "${PERL_CORE_DIR}/perl.h")
      set(ICES_HAVE_PERL 1)
      separate_arguments(PERL_CCOPTS_LIST UNIX_COMMAND "${PERL_CCOPTS}")
      separate_arguments(PERL_SYSLIBS_LIST UNIX_COMMAND "${PERL_SYSLIBS}")
      # Perl's headers are C; keep them out of the C++ translation units.
      add_library(ices_perl_glue STATIC src/perl_glue.c)
      target_compile_options(ices_perl_glue PRIVATE ${PERL_CCOPTS_LIST} -w)
      target_compile_definitions(ices_perl_glue PRIVATE ICES_MODULEDIR="${ICES_MODULEDIR}")
      target_include_directories(ices_perl_glue PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
      target_link_libraries(ices_perl_glue PUBLIC ${PERL_LIBRARY} ${PERL_SYSLIBS_LIST})
      target_sources(ices PRIVATE src/playlist_perl.cc)
      target_link_libraries(ices PRIVATE ices_perl_glue)
      # Export symbols so XS modules loaded by DynaLoader can resolve libperl.
      target_link_options(ices PRIVATE -Wl,-E)
      message(STATUS "Perl playlist support: yes (${PERL_LIBRARY})")
    else()
      message(STATUS "Perl playlist support: no (perl headers or libperl not found)")
    endif()
  endif()
endif()

target_compile_definitions(ices PRIVATE
  ICES_PYTHON_ENABLED=${ICES_HAVE_PYTHON} ICES_PERL_ENABLED=${ICES_HAVE_PERL})

configure_file(src/build_config.h.in "${CMAKE_CURRENT_BINARY_DIR}/src/build_config.h" @ONLY)

install(TARGETS ices RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(ICES_EXAMPLESDIR)
  install(FILES conf/ices.conf.dist conf/ices.py.dist conf/ices.pm.dist DESTINATION ${ICES_EXAMPLESDIR})
else()
  install(FILES conf/ices.conf.dist DESTINATION ${CMAKE_INSTALL_SYSCONFDIR})
  install(FILES conf/ices.py.dist conf/ices.pm.dist DESTINATION ${ICES_MODULEDIR})
endif()
install(FILES doc/ices.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

enable_testing()
add_test(NAME integration
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:ices>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests)
