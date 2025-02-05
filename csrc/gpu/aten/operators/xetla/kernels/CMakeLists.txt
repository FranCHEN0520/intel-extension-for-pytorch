set(TARGET xetla_kernels)
set(IPEX_INCLUDE_DIRS
        ${TORCH_INCLUDE_DIRS}
        ${SYCL_INCLUDE_DIR}
        ${IPEX_GPU_ROOT_DIR}
        ${IPEX_GPU_ATEN_SRC_DIR}
        ${IPEX_CSRC_ROOT_DIR}
        ${BUILD_IPEX_GPU_ATEN_GENERATED})

# link openblas
set(XETLA_KERNEL_FLAGS ${XETLA_KERNEL_FLAGS} -fsycl)

if (USE_AOT_DEVLIST)
  set(XETLA_KERNEL_FLAGS ${XETLA_KERNEL_FLAGS} -fsycl-targets=spir64_gen)
endif()

# For large registers mode, enable 256 registers for kernels
set(XETLA_OFFLINE_OPTIONS "-doubleGRF")
set(XETLA_OFFLINE_OPTIONS "${XETLA_OFFLINE_OPTIONS} -vc-disable-indvars-opt")
set(XETLA_OFFLINE_OPTIONS "${XETLA_OFFLINE_OPTIONS} -vc-codegen")
# For registers usage verbose at AOT
set(XETLA_OFFLINE_OPTIONS "${XETLA_OFFLINE_OPTIONS} -Xfinalizer -printregusage")
# Enable bank conflict reduction.
set(XETLA_OFFLINE_OPTIONS "${XETLA_OFFLINE_OPTIONS} -Xfinalizer -enableBCR")
# Optimization to reduce the tokens used for DPAS instruction.
set(XETLA_OFFLINE_OPTIONS "${XETLA_OFFLINE_OPTIONS} -Xfinalizer -DPASTokenReduction")

set(XETLA_KERNEL_FLAGS ${XETLA_KERNEL_FLAGS} -Xs )
if (USE_AOT_DEVLIST)
  set(XETLA_KERNEL_FLAGS ${XETLA_KERNEL_FLAGS} "-device pvc -options '${XETLA_OFFLINE_OPTIONS}'")
else()
  set(XETLA_KERNEL_FLAGS ${XETLA_KERNEL_FLAGS} "${XETLA_OFFLINE_OPTIONS}")
endif()


cmake_minimum_required(VERSION 3.16)
set(CMAKE_CXX_STANDARD 20)

FILE(GLOB_RECURSE kernels_src *.cpp)

SET(BLA_VENDOR INTEL10_64LP)

# Xetla kernel apply different offline compiler options than framework.
# If framework link XeTLA as static lib, link options would conflict now.
# Temporarily adopting shared lib, will fix it soon.
add_library(${TARGET} SHARED ${kernels_src})

# set new_xmain defult status to "on" to match oneapi2300.1.0
target_compile_definitions(${TARGET} PUBLIC XETPP_NEW_XMAIN)
message(STATUS "XETPP_NEW_XMAIN is set to " $ENV{NEW_XMAIN})

set(XETLA_INCLUDE_DIR "${IPEX_THIRD_PARTY_ROOT}/xetla/include")
if (USE_XETLA_SRC)
  set(XETLA_INCLUDE_DIR "${USE_XETLA_SRC}/include")
endif()

find_file(XETLA_HEADER NAMES xetla.hpp PATHS ${XETLA_INCLUDE_DIR} PATH_SUFFIXES NO_DEFAULT_PATH)
if(NOT XETLA_HEADER)
  message(FATAL_ERROR "XeTLA header ${XETLA_HEADER} not found in ${XETLA_INCLUDE_DIR}")
endif()

target_include_directories(${TARGET} PUBLIC ${XETLA_INCLUDE_DIR} )
target_include_directories(${TARGET} PUBLIC ${IPEX_INCLUDE_DIRS} )

target_link_options(${TARGET} PRIVATE ${XETLA_KERNEL_FLAGS})
if (USE_AOT_DEVLIST)
  target_compile_options(${TARGET} PRIVATE -fsycl -fsycl-targets=spir64_gen)
else()
  target_compile_options(${TARGET} PRIVATE -fsycl)
endif()
