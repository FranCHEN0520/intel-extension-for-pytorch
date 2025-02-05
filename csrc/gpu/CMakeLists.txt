set(IPEX_GPU_ATEN_SRC_DIR "${IPEX_GPU_ROOT_DIR}/aten")
set(IPEX_GPU_ATEN_CORE_DIR "${IPEX_GPU_ATEN_SRC_DIR}/core")
set(IPEX_GPU_RUNTIME_SRC_DIR "${IPEX_GPU_ROOT_DIR}/runtime")
set(IPEX_GPU_UTILS_SRC_DIR "${IPEX_GPU_ROOT_DIR}/utils")
set(IPEX_GPU_PROFILER_SRC_DIR "${IPEX_GPU_ROOT_DIR}/profiler")

if (WINDOWS)
  set(LINK_LIBRARY_PREFIX "")
  set(LINK_LIBRARY_SUFFIX ".lib")
else()
  set(LINK_LIBRARY_PREFIX ${CMAKE_SHARED_LIBRARY_PREFIX})
  set(LINK_LIBRARY_SUFFIX ${CMAKE_SHARED_LIBRARY_SUFFIX})
endif()

list(APPEND GPU_TORCH_LIBS ${TORCH_INSTALL_PREFIX}/lib/${LINK_LIBRARY_PREFIX}torch${LINK_LIBRARY_SUFFIX})
list(APPEND GPU_TORCH_LIBS ${TORCH_INSTALL_PREFIX}/lib/${LINK_LIBRARY_PREFIX}torch_cpu${LINK_LIBRARY_SUFFIX})
list(APPEND GPU_TORCH_LIBS ${TORCH_INSTALL_PREFIX}/lib/${LINK_LIBRARY_PREFIX}c10${LINK_LIBRARY_SUFFIX})

# sources
set(IPEX_GPU_GEN_FILES)
set(IPEX_GPU_GEN_TARGET)
set(BUILD_IPEX_GPU_ATEN_GENERATED)
include(${PROJECT_SOURCE_DIR}/cmake/gpu/Codegen.cmake)

set(IPEX_ATEN_CORE_SRCS)
set(IPEX_ATEN_OPS_SRCS)
add_subdirectory(${IPEX_GPU_ATEN_SRC_DIR})

set(IPEX_RUNTIME_SRCS)
add_subdirectory(${IPEX_GPU_RUNTIME_SRC_DIR})

set(IPEX_JIT_SRCS)
add_subdirectory(${IPEX_GPU_ROOT_DIR}/jit)

set(IPEX_UTILS_SRCS)
add_subdirectory(${IPEX_GPU_ROOT_DIR}/utils)

set(IPEX_DISTRIBUTED_SRCS)
add_subdirectory(${IPEX_GPU_ROOT_DIR}/distributed)

set(IPEX_PROFILER_INCLUDES)
set(IPEX_PROFILER_SRCS)
add_subdirectory(${IPEX_GPU_PROFILER_SRC_DIR})

if(USE_XETLA)
  set(IPEX_GPU_XETLA_DIR "${IPEX_GPU_ROOT_DIR}/aten/operators/xetla")
  add_subdirectory(${IPEX_GPU_XETLA_DIR})
endif()

if(NOT DEFINED SYCL_INCLUDE_DIR)
  message(FATAL_ERROR, "Cannot find SYCL include directory")
endif()

set(IPEX_INCLUDE_DIRS
        ${TORCH_INCLUDE_DIRS}
        ${SYCL_INCLUDE_DIR}
        ${IPEX_GPU_ROOT_DIR}
        ${IPEX_GPU_ATEN_SRC_DIR}
        ${IPEX_CSRC_ROOT_DIR}
        ${BUILD_IPEX_GPU_ATEN_GENERATED}
        ${IPEX_PROFILER_INCLUDES})

set(IPEX_GPU_CORE_SRC
        ${IPEX_JIT_SRCS}
        ${IPEX_DISTRIBUTED_SRCS}
        ${IPEX_ATEN_CORE_SRCS}
        ${IPEX_RUNTIME_SRCS}
        ${IPEX_GPU_GEN_FILES}
        ${IPEX_UTILS_SRCS}
        ${IPEX_PROFILER_SRCS})

if(BUILD_SEPARATE_OPS)
  set(IPEX_GPU_OP_LIBRARIES)
  foreach(op_src ${IPEX_ATEN_OPS_SRCS})
      get_filename_component(obj ${op_src} NAME_WLE REALPATH)
      set(op_lib intel-ext-pt-gpu-op-${obj})
      add_library(${op_lib} SHARED ${op_src})
      target_include_directories(${op_lib} PUBLIC ${IPEX_INCLUDE_DIRS})
      target_compile_options(${op_lib} PRIVATE ${IPEX_SYCL_KERNEL_FLAGS})
      target_link_directories(${op_lib} PUBLIC ${SYCL_LIBRARY_DIR})
      target_link_libraries(${op_lib} PUBLIC ${GPU_TORCH_LIBS})
      target_link_options(${op_lib} PRIVATE ${IPEX_SYCL_LINK_FLAGS})
      list(APPEND IPEX_GPU_OP_LIBRARIES ${op_lib})
  endforeach()

else()
  list(APPEND IPEX_GPU_CORE_SRC ${IPEX_ATEN_OPS_SRCS})
endif()

add_library(intel-ext-pt-gpu SHARED ${IPEX_GPU_CORE_SRC})

target_include_directories(intel-ext-pt-gpu PUBLIC ${IPEX_INCLUDE_DIRS})
target_compile_options(intel-ext-pt-gpu PRIVATE ${IPEX_SYCL_KERNEL_FLAGS})

target_link_directories(intel-ext-pt-gpu PUBLIC ${SYCL_LIBRARY_DIR})
target_link_options(intel-ext-pt-gpu PRIVATE ${IPEX_SYCL_LINK_FLAGS})

add_dependencies(intel-ext-pt-gpu IPEX_GPU_GEN_TARGET)
include(${PROJECT_SOURCE_DIR}/cmake/ClangFormat.cmake)
if(CLANG_FORMAT)
  file(GLOB_RECURSE ALL_GPU_NATIVE_CSRC_FILES ${IPEX_GPU_ROOT_DIR}/**.[ch] ${IPEX_GPU_ROOT_DIR}/**.[ch]pp)
  add_custom_target(CL_FORMAT_GPU_NATIVE_CSRC COMMAND ${CLANG_FORMAT_EXEC} -i -style=file ${ALL_GPU_NATIVE_CSRC_FILES})
  add_dependencies(intel-ext-pt-gpu CL_FORMAT_GPU_NATIVE_CSRC)
endif()

if(BUILD_SEPARATE_OPS)
  target_link_libraries(intel-ext-pt-gpu PUBLIC ${GPU_TORCH_LIBS} ${IPEX_GPU_OP_LIBRARIES})
else()
  target_link_libraries(intel-ext-pt-gpu PUBLIC ${GPU_TORCH_LIBS})
endif()

if(USE_XETLA)
  target_link_libraries(intel-ext-pt-gpu PUBLIC xetla_kernels)
endif()

set(IPEX_COMPILE_DEFINITIONS)
if(USE_PERSIST_STREAM)
  list(APPEND IPEX_COMPILE_DEFINITIONS "USE_PERSIST_STREAM")
endif()

if(BUILD_SIMPLE_TRACE)
  list(APPEND IPEX_COMPILE_DEFINITIONS "BUILD_SIMPLE_TRACE")
endif()

if(BUILD_INTERNAL_DEBUG)
  list(APPEND IPEX_COMPILE_DEFINITIONS "BUILD_INTERNAL_DEBUG")
endif()

if (USE_QUEUE_BARRIER)
  list(APPEND IPEX_COMPILE_DEFINITIONS "USE_QUEUE_BARRIER")
endif()

if (USE_PRIMITIVE_CACHE)
  # Enable FRAMEWORK primitive cache
  list(APPEND IPEX_COMPILE_DEFINITIONS "USE_PRIMITIVE_CACHE")
endif()

if (USE_SCRATCHPAD_MODE)
  # Enable oneDNN scratchpad user mode
  list(APPEND IPEX_COMPILE_DEFINITIONS "USE_SCRATCHPAD_MODE")
endif()

if(USE_XETLA)
  list(APPEND IPEX_COMPILE_DEFINITIONS "USE_XETLA")
endif()

if (USE_CHANNELS_LAST_1D)
  list(APPEND IPEX_COMPILE_DEFINITIONS "USE_CHANNELS_LAST_1D")
endif()

if (USE_MULTI_CONTEXT)
  list(APPEND IPEX_COMPILE_DEFINITIONS "USE_MULTI_CONTEXT")
endif()

if (USE_PROFILER)
  # Enable Profiler
  list(APPEND IPEX_COMPILE_DEFINITIONS "USE_PROFILER")
endif()

if (USE_KINETO)
  if (USE_ONETRACE)
    find_package(oneTrace QUIET)
    if (ONETRACE_FOUND)
      target_link_libraries(intel-ext-pt-gpu PUBLIC ${ONETRACE_LIBRARY})
      target_include_directories(intel-ext-pt-gpu PUBLIC ${ONETRACE_INCLUDE_DIRS})
      list(APPEND IPEX_COMPILE_DEFINITIONS "USE_ONETRACE")
      set_target_properties(intel-ext-pt-gpu PROPERTIES USE_ONETRACE ON)
      set_target_properties(intel-ext-pt-gpu PROPERTIES ONETRACE_INCLUDE_DIRS "${ONETRACE_INCLUDE_DIRS}")
    else()
      message(WARNING "WARNING: Cannot find onetrace! Continue to build without onetrace!")
      set_target_properties(intel-ext-pt-gpu PROPERTIES USE_ONETRACE OFF)
    endif()
  endif()
  if (USE_PTI)
    message(WARNING "WARNING: PTI library has not been ready! Continue to build without PTI!")
    set_target_properties(intel-ext-pt-gpu PROPERTIES USE_PTI OFF)
  endif()
  list(APPEND IPEX_COMPILE_DEFINITIONS "USE_KINETO")

  find_package(fmt QUIET)
  if (FMT_FOUND)
    target_link_libraries(intel-ext-pt-gpu PRIVATE fmt::fmt-header-only)
    target_include_directories(intel-ext-pt-gpu PRIVATE ${FMT_INCLUDE_DIR})
    set_target_properties(intel-ext-pt-gpu PROPERTIES FMT_INCLUDE_DIR "${FMT_INCLUDE_DIR}")
  else()
    message(FATAL_ERROR "ERROR: Cannot build fmt::fmt-header-only! Cannot build USE_KINETO without FMT!")
  endif()
  target_compile_options(intel-ext-pt-gpu PRIVATE "-DKINETO_NAMESPACE=libkineto")
endif()

if (BUILD_JIT_QUANTIZATION_SAVE)
  list(APPEND IPEX_COMPILE_DEFINITIONS "BUILD_JIT_QUANTIZATION_SAVE")
endif()

if (USE_SPLIT_FP64_LOOPS)
  list(APPEND IPEX_COMPILE_DEFINITIONS "USE_SPLIT_FP64_LOOPS")
endif()

list(APPEND IPEX_COMPILE_DEFINITIONS "BUILD_IPEX_MAIN_LIB")

if (USE_ONEMKL)
    find_package(oneMKL QUIET)
    if (ONEMKL_FOUND)
        target_link_libraries(intel-ext-pt-gpu PUBLIC ${ONEMKL_GPU_LIBS})
        target_include_directories(intel-ext-pt-gpu PUBLIC ${ONEMKL_INCLUDE_DIR})
        list(APPEND IPEX_COMPILE_DEFINITIONS "USE_ONEMKL")
        if(BUILD_SEPARATE_OPS)
            foreach(op_lib ${IPEX_GPU_OP_LIBRARIES})
                target_link_libraries(${op_lib} PUBLIC ${ONEMKL_GPU_LIBS})
                target_include_directories(${op_lib} PUBLIC ${ONEMKL_INCLUDE_DIR})
            endforeach()
        endif()
        set_target_properties(intel-ext-pt-gpu PROPERTIES USE_ONEMKL ON)
        set_target_properties(intel-ext-pt-gpu PROPERTIES ONEMKL_INCLUDE_DIR "${ONEMKL_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR "Cannot find oneMKL!")
    endif()
endif()

find_package(spdlog QUIET)

if(SPDLOG_FOUND) 
if(USE_XETLA)
  target_link_libraries(xetla_kernels PUBLIC spdlog::spdlog_header_only)
  target_include_directories(xetla_kernels PUBLIC ${SPDLOG_INCLUDE_DIRS})
endif()
  target_link_libraries(intel-ext-pt-gpu PUBLIC spdlog::spdlog_header_only)
  target_include_directories(intel-ext-pt-gpu PUBLIC ${SPDLOG_INCLUDE_DIRS})
  if(BUILD_SEPARATE_OPS)
    foreach(op_lib ${IPEX_GPU_OP_LIBRARIES})
      target_link_libraries(${op_lib} PUBLIC spdlog::spdlog_header_only)
      target_include_directories(${op_lib} PUBLIC ${SPDLOG_INCLUDE_DIRS})
    endforeach()
  endif()
else()
  message(FATAL_ERROR "Error: Cannot find spdlog")
endif()

# XXX: Please make sure to find oneMKL before oneDNN
find_package(oneDNN QUIET)
if(ONEDNN_FOUND)
  if(ONEDNN_EXTERNAL_LIB)
    target_link_libraries(intel-ext-pt-gpu PRIVATE ${ONEDNN_EXTERNAL_LIB})
  else()
    target_link_libraries(intel-ext-pt-gpu PRIVATE ${ONEDNN_LIBRARY})
  endif()
  target_include_directories(intel-ext-pt-gpu BEFORE PUBLIC ${ONEDNN_INCLUDE_DIR})
  set_target_properties(intel-ext-pt-gpu PROPERTIES ONEDNN_INCLUDE_DIR "${ONEDNN_INCLUDE_DIR}")
  if(ONEDNN_EXTERNAL_LIB)
    set_target_properties(intel-ext-pt-gpu PROPERTIES ONEDNN_LIBRARY "${ONEDNN_EXTERNAL_LIB}")
  else()
    set_target_properties(intel-ext-pt-gpu PROPERTIES ONEDNN_LIBRARY "${ONEDNN_LIBRARY}")
  endif()
  if(BUILD_SEPARATE_OPS)
    # Use same lib path with op libs to avoid link errors
    set(ONEDNN_LIB_DIR "${CMAKE_BINARY_DIR}/csrc/gpu")
    # Suppress soname to prevent from creating symbolic link
    set_target_properties(${ONEDNN_LIBRARY} PROPERTIES NO_SONAME ON
      LIBRARY_OUTPUT_DIRECTORY ${ONEDNN_LIB_DIR}
      RUNTIME_OUTPUT_DIRECTORY ${ONEDNN_LIB_DIR})
    foreach(op_lib ${IPEX_GPU_OP_LIBRARIES})
      if(ONEDNN_EXTERNAL_LIB)
        target_link_libraries(${op_lib} PRIVATE ${ONEDNN_EXTERNAL_LIB})
      else()
        target_link_libraries(${op_lib} PRIVATE ${ONEDNN_LIBRARY})
      endif()
      target_include_directories(${op_lib} BEFORE PUBLIC ${ONEDNN_INCLUDE_DIR})
    endforeach()
  endif()
else()
    message(FATAL_ERROR "Cannot find oneDNN")
endif()

target_compile_definitions(intel-ext-pt-gpu PUBLIC ${IPEX_COMPILE_DEFINITIONS})
if(BUILD_SEPARATE_OPS)
  foreach(op_lib ${IPEX_GPU_OP_LIBRARIES})
      target_compile_definitions(${op_lib} PUBLIC ${IPEX_COMPILE_DEFINITIONS})
  endforeach()
endif()

if(BUILD_STRIPPED_BIN)
  set_target_properties(intel-ext-pt-gpu PROPERTIES LINK_FLAGS_RELEASE -s)
  if(BUILD_SEPARATE_OPS)
    foreach(op_lib ${IPEX_GPU_OP_LIBRARIES})
        set_target_properties(${op_lib} PROPERTIES LINK_FLAGS_RELEASE -s)
    endforeach()
  endif()
endif()

set(LIBRARIES_INSTALL)

if(BUILD_SEPARATE_OPS)
  # oneDNN is linked statically if not BUILD_SEPARATE_OPS, otherwise is linked dynamically
  if(ONEDNN_LIBRARY_FILES)
    install(FILES ${ONEDNN_LIBRARY_FILES}
      DESTINATION ${CMAKE_INSTALL_LIBDIR})
    list(APPEND LIBRARIES_INSTALL ${ONEDNN_LIBRARY_FILES})
  else()
    install(TARGETS ${ONEDNN_LIBRARY}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    list(APPEND LIBRARIES_INSTALL ${ONEDNN_LIBRARY})
  endif()

  foreach(op_lib ${IPEX_GPU_OP_LIBRARIES})
    install(TARGETS ${op_lib}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    list(APPEND LIBRARIES_INSTALL ${op_lib})
  endforeach()
endif()

if(USE_XETLA)
  install(TARGETS xetla_kernels
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  list(APPEND LIBRARIES_INSTALL xetla_kernels)
endif()

if(USE_KINETO)
  if(USE_ONETRACE)
    # static library
    install(TARGETS ${ONETRACE_LIBRARY}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  elseif(USE_PTI)
    message(WARNING "WARNING: PTI library has not been ready! Continue to build without PTI!")
  endif()
endif()

install(TARGETS intel-ext-pt-gpu
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
list(APPEND LIBRARIES_INSTALL intel-ext-pt-gpu)

file(WRITE "${LIBRARIES_INSTALL_FILE_PREFIX}_gpu" "${LIBRARIES_INSTALL}")
