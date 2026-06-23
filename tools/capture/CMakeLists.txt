# Statebox Capture Tool — offline DSP core first (CLAUDE.md §10).
# Console app for now; juce_audio_devices (live I/O) and a GUI are layered on later.

juce_add_console_app(StateboxCapture
    PRODUCT_NAME "Statebox Capture")

target_sources(StateboxCapture PRIVATE
    main.cpp)

target_compile_definitions(StateboxCapture PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0)

target_link_libraries(StateboxCapture PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
