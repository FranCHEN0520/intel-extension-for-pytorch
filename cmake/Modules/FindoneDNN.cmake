# - Try to find oneDNN
#
# The following variables are optionally searched for defaults
#  MKL_FOUND             : set to true if a library implementing the CBLAS interface is found
#
# The following are set after configuration is done:
#  ONEDNN_FOUND          : set to true if oneDNN is found.
#  ONEDNN_INCLUDE_DIR    : path to oneDNN include dir.
#  ONEDNN_LIBRARIES      : list of libraries for oneDNN
#
# The following variables are used:
#  ONEDNN_USE_NATIVE_ARCH : Whether native CPU instructions should be used in ONEDNN. This should be turned off for
#  general packaging to avoid incompatible CPU instructions. Default: OFF.

IF (NOT ONEDNN_FOUND)

SET(ONEDNN_LIBRARIES)
SET(ONEDNN_INCLUDE_DIR)

SET(ONEDNN_ROOT "${PROJECT_SOURCE_DIR}/third_party/oneDNN")

FIND_PATH(ONEDNN_INCLUDE_DIR dnnl.hpp dnnl.h PATHS ${ONEDNN_ROOT} PATH_SUFFIXES include)
IF (NOT ONEDNN_INCLUDE_DIR)
  EXECUTE_PROCESS(COMMAND git${CMAKE_EXECUTABLE_SUFFIX} submodule update --init oneDNN WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/third_party)
  FIND_PATH(ONEDNN_INCLUDE_DIR dnnl.hpp dnnl.h PATHS ${ONEDNN_ROOT} PATH_SUFFIXES include)
ENDIF(NOT ONEDNN_INCLUDE_DIR)

IF (NOT ONEDNN_INCLUDE_DIR)
  MESSAGE(STATUS "oneDNN source files not found!")
  RETURN()
ENDIF(NOT ONEDNN_INCLUDE_DIR)

IF (USE_SYCL)
  SET(DNNL_CPU_RUNTIME "SYCL" CACHE STRING "oneDNN cpu backend" FORCE)
  SET(DNNL_GPU_RUNTIME "SYCL" CACHE STRING "oneDNN gpu backend" FORCE)
  SET(DNNL_ENABLE_PRIMITIVE_CACHE TRUE CACHE BOOL "oneDNN sycl primitive cache" FORCE)
  if(USE_USM)
    add_definitions(-DDNNL_USE_DPCPP_USM)
  endif()
ENDIF()

IF(ONEDNN_USE_NATIVE_ARCH)  # Disable HostOpts in oneDNN unless ONEDNN_USE_NATIVE_ARCH is set.
  SET(DNNL_ARCH_OPT_FLAGS "HostOpts" CACHE STRING "" FORCE)
ELSE()
  IF(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    SET(DNNL_ARCH_OPT_FLAGS "-msse4" CACHE STRING "" FORCE)
  ELSE()
    SET(DNNL_ARCH_OPT_FLAGS "" CACHE STRING "" FORCE)
  ENDIF()
ENDIF()

SET(DNNL_BUILD_TESTS FALSE CACHE BOOL "build with oneDNN tests" FORCE)
SET(DNNL_BUILD_EXAMPLES FALSE CACHE BOOL "build with oneDNN examples" FORCE)
SET(DNNL_ENABLE_CONCURRENT_EXEC TRUE CACHE BOOL "multi-thread primitive execution" FORCE)
SET(DNNL_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)

ADD_SUBDIRECTORY(${ONEDNN_ROOT} EXCLUDE_FROM_ALL)
IF(NOT TARGET dnnl)
  MESSAGE("Failed to include oneDNN target")
  RETURN()
ENDIF(NOT TARGET dnnl)
IF(NOT APPLE AND CMAKE_COMPILER_IS_GNUCC)
  TARGET_COMPILE_OPTIONS(dnnl PRIVATE -Wno-uninitialized)
  TARGET_COMPILE_OPTIONS(dnnl PRIVATE -Wno-strict-overflow)
  TARGET_COMPILE_OPTIONS(dnnl PRIVATE -Wno-error=strict-overflow)
ENDIF(NOT APPLE AND CMAKE_COMPILER_IS_GNUCC)
TARGET_COMPILE_OPTIONS(dnnl PRIVATE -Wno-tautological-compare)
GET_TARGET_PROPERTY(DNNL_INCLUDES dnnl INCLUDE_DIRECTORIES)
LIST(APPEND ONEDNN_INCLUDE_DIR ${DNNL_INCLUDES})
LIST(APPEND ONEDNN_LIBRARIES dnnl)

SET(ONEDNN_FOUND TRUE)
MESSAGE(STATUS "Found oneDNN: TRUE")

ENDIF(NOT ONEDNN_FOUND)
