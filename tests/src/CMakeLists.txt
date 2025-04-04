################################################################################
# Project:  NextGIS QGIS
# Purpose:  CMake build scripts
################################################################################
# Copyright (C) 2017-2022, NextGIS <info@nextgis.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
################################################################################

cmake_policy(SET CMP0002 OLD)

if (ENABLE_TESTS)
  macro (add_qgis_test TESTSRC)
      set(options OPTIONAL)
      set(oneValueArgs MODULE LABELS)
      set(multiValueArgs LINKEDLIBRARIES DEPENDENCIES)
      cmake_parse_arguments(ARG_QGIS_TEST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

      set (TESTNAME ${TESTSRC})
      string(REPLACE "test" "" TESTNAME ${TESTNAME})
      string(REPLACE "qgs" "" TESTNAME ${TESTNAME})
      string(REPLACE ".cpp" "" TESTNAME ${TESTNAME})
      set (TESTNAME  "test_${ARG_QGIS_TEST_MODULE}_${TESTNAME}")
      add_executable(${TESTNAME} ${TESTSRC} ${util_SRCS})
      # add_custom_target(${TESTNAME}moc ALL DEPENDS ${${TESTNAME}_MOC_SRCS})
      target_compile_features(${TESTNAME} PRIVATE cxx_std_17)
      set_target_properties(${TESTNAME} PROPERTIES AUTORCC TRUE)
      target_link_libraries(${TESTNAME} ${ARG_QGIS_TEST_LINKEDLIBRARIES})
      target_link_libraries(${TESTNAME} qgis_test)
      add_test(${TESTNAME} ${CMAKE_BINARY_DIR}/output/bin/${TESTNAME} -maxwarnings 10000)
      if (DEFINED ARG_QGIS_TEST_DEPENDENCIES)
        add_dependencies(${TESTNAME} ${ARG_QGIS_TEST_DEPENDENCIES})
      endif()
      if (DEFINED ARG_QGIS_TEST_LABELS)
        set_tests_properties(${TESTNAME} PROPERTIES LABELS ${ARG_QGIS_TEST_LABELS})
      endif()

      if (NOT PROJ_DB_POSITION)
        string(REGEX REPLACE ".*/(.*)" "\\1" LAST_PART_DB_POSITION "${PROJ_INCLUDE_DIR}")
        if(${LAST_PART_DB_POSITION} STREQUAL "include")
          string(REPLACE "/include" "/share/proj" PROJ_DB_POSITION "${PROJ_INCLUDE_DIR}")
        endif()
        message("PROJ_LIB database position for CTest set as ${PROJ_DB_POSITION}")
      endif()
      set_tests_properties(${TESTNAME} PROPERTIES ENVIRONMENT "PROJ_LIB=${PROJ_DB_POSITION}")
    endmacro()

  #############################################################
  # Compiler defines

  # This define is used for tests that need to locate the test
  # data under tests/testdata in the qgis source tree.
  # the TEST_DATA_DIR variable is set in the top level CMakeLists.txt
  add_definitions(-DTEST_DATA_DIR="${TEST_DATA_DIR}")

  add_definitions(-DINSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}")

  # libraries
  # enable postgresql tests
  set (ENABLE_PGTEST FALSE CACHE BOOL "Enable PostgreSQL provider tests")
  if ( ENABLE_PGTEST )
      add_definitions(-DENABLE_PGTEST)
  endif()

  # because of htonl
  if (WIN32)
      set(PLATFORM_LIBRARIES wsock32)
  endif()

  # Since the tests are not actually installed, but rather
  # run directly from the build/src/tests dir we need to
  # ensure the qgis libs can be found.
  # if (APPLE)
    # For Mac OS X, the executable must be at the root of the bundle's executable folder
    #  set (CMAKE_INSTALL_NAME_DIR @executable_path/../../../src/core)
  # endif()

  add_subdirectory(core)
  if (WITH_GUI)
      add_subdirectory(gui)
  endif()
  if (WITH_3D)
      add_subdirectory(3d)
  endif()
  if (WITH_ANALYSIS)
      add_subdirectory(analysis)
      #add_subdirectory(geometry_checker)
  endif()
  if (WITH_AUTH)
      add_subdirectory(auth)
  endif()
  add_subdirectory(providers)
  if (WITH_DESKTOP)
      add_subdirectory(app)
  endif()
  add_subdirectory(native)
  if (WITH_BINDINGS)
      add_subdirectory(python)
  endif()
  if (WITH_QUICK)
      add_subdirectory(quickgui)
  endif()
  # if (WITH_SERVER)
  #     add_subdirectory(server)
  # endif()
endif()

get_target_property(_PATH Qt5::qmake IMPORTED_LOCATION)
get_filename_component(QT_BIN_DIR ${_PATH} PATH)
get_filename_component(QT_ROOT_DIR ${QT_BIN_DIR} PATH)
set(QT5_TRANSLATION_DEVDIR ${QT_ROOT_DIR}/translations)
set(QT5_PLUGINS_DEVDIR ${QT_ROOT_DIR}/plugins)

configure_file(${CMAKE_SOURCE_DIR}/cmake/qt.conf.cmake.tmp.in ${BINARY_DIR}/qt.conf IMMEDIATE @ONLY)
