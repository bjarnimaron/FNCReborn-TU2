# fnc_content - installs the player's own title update and DLC packages (see fnc_content.cpp).
# Built by scripts/setup.ps1 against the ReXGlue SDK, before the game itself.
cmake_minimum_required(VERSION 3.25)
project(fnc_content LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED ENV{REXSDK})
    list(PREPEND CMAKE_PREFIX_PATH $ENV{REXSDK})
endif()
find_package(rexglue 0.10.0 REQUIRED CONFIG)

add_executable(fnc_content fnc_content.cpp)
target_link_libraries(fnc_content PRIVATE rex::runtime)
rexglue_apply_target_settings(fnc_content)
