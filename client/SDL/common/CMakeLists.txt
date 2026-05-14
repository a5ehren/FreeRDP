# FreeRDP: A Remote Desktop Protocol Implementation
# FreeRDP SDL Client
#
# Copyright 2024 Armin Novak <anovak@thincast.com>
# Copyright 2024 Thincast Technologies GmbH
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

include(CheckCXXSourceCompiles)

set(SDL_FILESYSTEM_LIB "")
set(SDL_FILESYSTEM_CHECK_SOURCE
    "
#if __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#elif __has_include(<experimental/filesystem>)
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
#error Could not find system header \"<filesystem>\" or \"<experimental/filesystem>\"
#endif

int main()
{
  auto path = fs::current_path();
  return path.empty();
}
")

check_cxx_source_compiles("${SDL_FILESYSTEM_CHECK_SOURCE}" SDL_FILESYSTEM_LINKS_WITHOUT_LIB)
if(NOT SDL_FILESYSTEM_LINKS_WITHOUT_LIB)
  set(SDL_CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES})
  set(CMAKE_REQUIRED_LIBRARIES stdc++fs)
  check_cxx_source_compiles("${SDL_FILESYSTEM_CHECK_SOURCE}" SDL_FILESYSTEM_LINKS_WITH_STDCXXFS)
  set(CMAKE_REQUIRED_LIBRARIES ${SDL_CMAKE_REQUIRED_LIBRARIES})
  if(SDL_FILESYSTEM_LINKS_WITH_STDCXXFS)
    set(SDL_FILESYSTEM_LIB stdc++fs)
  endif()
endif()

add_subdirectory(aad)
add_subdirectory(res)

set(SRCS sdl_prefs.hpp sdl_prefs.cpp scoped_guard.hpp sdl_common_utils.hpp sdl_common_utils.cpp)
if(WIN32)
  list(APPEND SRCS sdl_win32_console.hpp sdl_win32_console.cpp)
endif()

add_library(sdl-common-prefs STATIC ${SRCS})
target_link_libraries(sdl-common-prefs winpr freerdp)
if(SDL_FILESYSTEM_LIB)
  target_link_libraries(sdl-common-prefs ${SDL_FILESYSTEM_LIB})
endif()
set_property(TARGET sdl-common-prefs PROPERTY FOLDER "Client/Common")

if(BUILD_TESTING_INTERNAL OR BUILD_TESTING)
  add_subdirectory(test)
endif()
