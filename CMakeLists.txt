cmake_minimum_required(VERSION 3.22)

project(Statebox VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# JUCE is pulled in (pinned) at configure time. Bump the tag deliberately; do not
# track moving `main`. JUCE 8.x is required (JUCE 7 breaks under CMake 4.x).
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.14          # latest 8.x; older 8.0.x fail to build juceaide on recent macOS SDKs
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# --- Targets -----------------------------------------------------------------
# The Capture Tool was built first (see CLAUDE.md §10). The Milestone-0 offline
# renderer (Source/dsp engine + tools/render CLI) follows, validated against real
# captures. The JUCE plugin shell comes after the go/no-go gate.
enable_testing()
add_subdirectory(tools/capture)
add_subdirectory(tools/render)
