#===============================================================================
# Copyright 2019 Intel Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#===============================================================================

# The following are set after configuration is done:
#  FMT_FOUND            : set to true if fmt is found.
#  FMT_INCLUDE_DIR      : path to fmt include dir.
#===============================================================================


if(FMT_FOUND)
  return()
endif()

set(FMT_FOUND OFF)
set(FMT_INCLUDE_DIR)

set(THIRD_PARTY_DIR "${PROJECT_SOURCE_DIR}/third_party")
set(FMT_ROOT "${THIRD_PARTY_DIR}/fmt")

add_subdirectory(${FMT_ROOT} fmt EXCLUDE_FROM_ALL)
if (NOT TARGET fmt)
  message(FATAL_ERROR "Failed to include fmt target")
endif()
list(APPEND FMT_INCLUDE_DIR "${FMT_ROOT}/include")

set(FMT_FOUND ON)
message(STATUS "Found fmt: TRUE")
