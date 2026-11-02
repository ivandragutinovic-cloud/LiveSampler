cmake_minimum_required(VERSION 3.22)

# .exe radi i bez instaliranog Visual C++ runtime-a
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(LiveSampler VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE: ako imas JUCE na disku, prosledi -DJUCE_DIR=putanja,
# inace ga CMake sam skida sa GitHub-a.
if(DEFINED JUCE_DIR)
    add_subdirectory(${JUCE_DIR} juce_build)
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.15
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(LiveSampler
    COMPANY_NAME "LiveSampler"
    PRODUCT_NAME "Live Sampler"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    PLUGIN_MANUFACTURER_CODE Lvsm
    PLUGIN_CODE Lsp1
    FORMATS Standalone VST3)

juce_generate_juce_header(LiveSampler)

target_sources(LiveSampler PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/MappingEditor.cpp
    Source/SampleLibrary.cpp
    Source/SamplerPart.cpp
    Source/MasterEffects.cpp
    Source/Model.cpp
    Source/Pages.cpp
    Source/KronosPcg.cpp)

target_compile_definitions(LiveSampler PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_ASIO=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)

if(MSVC)
    target_compile_options(LiveSampler PRIVATE /utf-8)
endif()

target_link_libraries(LiveSampler
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Kronos Autosampler: poseban mali program za presemplovanje fabrickih ROM multisemplova
juce_add_gui_app(KronosAutosampler
    COMPANY_NAME "LiveSampler"
    PRODUCT_NAME "Kronos Autosampler")

juce_generate_juce_header(KronosAutosampler)

target_sources(KronosAutosampler PRIVATE
    Autosampler/Main.cpp
    Source/KronosPcg.cpp)

target_include_directories(KronosAutosampler PRIVATE Source)

target_compile_definitions(KronosAutosampler PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_ASIO=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)

if(MSVC)
    target_compile_options(KronosAutosampler PRIVATE /utf-8)
endif()

target_link_libraries(KronosAutosampler
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
