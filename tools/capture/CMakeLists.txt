# Statebox Capture Tool — offline DSP core first (CLAUDE.md §10).
# JUCE modules are compiled per-target, so the shared core sources are added to
# each target rather than a static lib (avoids JUCE module double-compilation).

set(CAPTURE_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/core/SweepSynth.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Deconvolution.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/HarmonicSeparation.cpp)

set(CAPTURE_JUCE_MODULES
    juce::juce_dsp
    juce::juce_audio_formats
    juce::juce_audio_basics
    juce::juce_core)

# --- Console app -------------------------------------------------------------
juce_add_console_app(StateboxCapture
    PRODUCT_NAME "Statebox Capture")

target_sources(StateboxCapture PRIVATE main.cpp ${CAPTURE_CORE_SOURCES})
target_include_directories(StateboxCapture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(StateboxCapture PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0)
target_link_libraries(StateboxCapture PRIVATE
    ${CAPTURE_JUCE_MODULES}
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# --- Tests -------------------------------------------------------------------
juce_add_console_app(StateboxCaptureTests
    PRODUCT_NAME "Statebox Capture Tests")

target_sources(StateboxCaptureTests PRIVATE tests/CoreTests.cpp ${CAPTURE_CORE_SOURCES})
target_include_directories(StateboxCaptureTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(StateboxCaptureTests PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0)
target_link_libraries(StateboxCaptureTests PRIVATE
    ${CAPTURE_JUCE_MODULES}
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

add_test(NAME core_tests COMMAND StateboxCaptureTests)
