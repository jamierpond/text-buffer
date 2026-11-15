cmake_minimum_required(VERSION 3.31)

set(CMAKE_PROJECT_TOP_LEVEL_INCLUDES ${CMAKE_CURRENT_LIST_DIR}/cmake/lockstep.cmake)

project(TextBuffer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# A consumer that already has its own set_strict_warnings_level keeps it.
if (NOT COMMAND set_strict_warnings_level)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/StrictWarnings.cmake)
endif ()

# Only eacp's Core is used (its string helpers), so its graphics stay unbuilt.
# A consumer that already brings eacp keeps its own.
if (NOT TARGET eacp-core)
    lockstep_add_package(
            NAME eacp
            GITHUB_REPOSITORY eyalamirmusic/eacp
            GIT_TAG develop
            SYSTEM YES
            OPTIONS
                "EACP_BUILD_GRAPHICS OFF"
                "EACP_ENABLE_EXAMPLES OFF"
                "EACP_ENABLE_TESTS OFF")
endif ()

if (PROJECT_IS_TOP_LEVEL)
    lockstep_add_package(
            NAME NanoTest
            GITHUB_REPOSITORY eyalamirmusic/NanoTest
            GIT_TAG main
            SYSTEM YES)

    # Set after the packages, so only this project's own targets build with -Werror.
    set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
endif ()

add_subdirectory(Lib/TextBuffer)
add_subdirectory(Lib/VimEditor)

if (PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_subdirectory(Tests)
endif ()
