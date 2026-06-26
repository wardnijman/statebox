# Statebox offline renderer — Milestone 0 (CLAUDE.md §10).
# Builds the real engine (Source/dsp) driven by an offline CLI, reusing the .dcpack
# reader from the capture core. Engine modules are compiled per-target (JUCE modules
# don't like double compilation in a shared lib).

set(ENGINE_SOURCES
    ${CMAKE_SOURCE_DIR}/Source/dsp/Convolver.cpp
    ${CMAKE_SOURCE_DIR}/Source/dsp/DynamicConvolutionEngine.cpp
    ${CMAKE_SOURCE_DIR}/Source/capture/RuntimeProfile.cpp
    ${CMAKE_SOURCE_DIR}/tools/capture/core/DcPack.cpp)

set(ENGINE_INCLUDES
    ${CMAKE_SOURCE_DIR}/Source
    ${CMAKE_SOURCE_DIR}/tools/capture/core)

set(ENGINE_JUCE_MODULES
    juce::juce_dsp
    juce::juce_audio_formats
    juce::juce_audio_basics
    juce::juce_core)

# --- Renderer ----------------------------------------------------------------
juce_add_console_app(StateboxRender PRODUCT_NAME "Statebox Render")
target_sources(StateboxRender PRIVATE main.cpp ${ENGINE_SOURCES})
target_include_directories(StateboxRender PRIVATE ${ENGINE_INCLUDES})
target_compile_definitions(StateboxRender PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(StateboxRender PRIVATE
    ${ENGINE_JUCE_MODULES}
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# --- Tests -------------------------------------------------------------------
juce_add_console_app(StateboxEngineTests PRODUCT_NAME "Statebox Engine Tests")
target_sources(StateboxEngineTests PRIVATE tests/EngineTests.cpp ${ENGINE_SOURCES})
target_include_directories(StateboxEngineTests PRIVATE ${ENGINE_INCLUDES})
target_compile_definitions(StateboxEngineTests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(StateboxEngineTests PRIVATE
    ${ENGINE_JUCE_MODULES}
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

add_test(NAME engine_tests COMMAND StateboxEngineTests)
