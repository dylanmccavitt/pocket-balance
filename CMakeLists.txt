cmake_minimum_required(VERSION 3.22)
project(PocketBalance VERSION 0.3.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(POCKET_BUILD_PLUGIN "Build native plugin formats" ON)
set(POCKET_JUCE_SOURCE_DIR "" CACHE PATH "Existing JUCE 8.0.15 source directory (avoids copying or downloading JUCE)")
include(CTest)
if(BUILD_TESTING)
  add_executable(PocketDspTests Tests/DspTests.cpp)
  target_include_directories(PocketDspTests PRIVATE Source)
  add_test(NAME PocketDspTests COMMAND PocketDspTests)
  add_executable(PocketClashTests Tests/ClashTests.cpp)
  target_include_directories(PocketClashTests PRIVATE Source)
  add_test(NAME PocketClashTests COMMAND PocketClashTests)
  add_executable(PocketIdentityTests Tests/IdentityTests.cpp)
  target_include_directories(PocketIdentityTests PRIVATE Source)
  add_test(NAME PocketIdentityTests COMMAND PocketIdentityTests)
endif()
if(POCKET_BUILD_PLUGIN)
  if(POCKET_JUCE_SOURCE_DIR)
    if(NOT EXISTS "${POCKET_JUCE_SOURCE_DIR}/CMakeLists.txt")
      message(FATAL_ERROR "POCKET_JUCE_SOURCE_DIR must point to a JUCE source directory containing CMakeLists.txt")
    endif()
    add_subdirectory("${POCKET_JUCE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/juce" EXCLUDE_FROM_ALL)
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE-8.0.15/CMakeLists.txt")
    add_subdirectory(third_party/JUCE-8.0.15 EXCLUDE_FROM_ALL)
  else()
    include(FetchContent)
    FetchContent_Declare(JUCE
      URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.15.tar.gz
      URL_HASH SHA256=0eafd736286c6e7bb6958d79fce18d42cb654b8e231890d327b7cece379bd156
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(JUCE)
  endif()
  set(POCKET_FORMATS VST3 Standalone)
  if(APPLE)
    list(APPEND POCKET_FORMATS AU)
  endif()
  juce_add_plugin(PocketBalance
    COMPANY_NAME "Pocket Audio"
    BUNDLE_ID "audio.pocket.balance"
    PLUGIN_MANUFACTURER_CODE Pckt
    PLUGIN_CODE Pb01
    PRODUCT_NAME "Pocket Balance"
    DESCRIPTION "A drum balance mixer with 16 separate inputs"
    FORMATS ${POCKET_FORMATS}
    VST3_CATEGORIES Fx Tools
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)
  target_sources(PocketBalance PRIVATE Source/PluginProcessor.cpp Source/PluginEditor.cpp)
  target_compile_definitions(PocketBalance PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
  target_link_libraries(PocketBalance PRIVATE juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
  foreach(FORMAT VST3 AU)
    if(TARGET PocketBalance_${FORMAT})
      if(FORMAT STREQUAL "AU")
        set(POCKET_BUNDLE_EXTENSION component)
      else()
        set(POCKET_BUNDLE_EXTENSION vst3)
      endif()
      add_custom_command(TARGET PocketBalance_${FORMAT} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
          "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
          "-DBUNDLE_PATH=${CMAKE_CURRENT_BINARY_DIR}/PocketBalance_artefacts/$<CONFIG>/${FORMAT}/Pocket Balance.${POCKET_BUNDLE_EXTENSION}"
          "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/PocketBalance-${FORMAT}-manifest.json"
          "-DVERSION=${PROJECT_VERSION}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/WriteManifest.cmake"
        VERBATIM)
    endif()
  endforeach()
  if(BUILD_TESTING)
    juce_add_console_app(PocketHostTests PRODUCT_NAME "Pocket Host Tests")
    get_target_property(POCKET_TEST_DEFINITIONS PocketHostTests COMPILE_DEFINITIONS)
    list(REMOVE_ITEM POCKET_TEST_DEFINITIONS JUCE_STANDALONE_APPLICATION=1)
    set_target_properties(PocketHostTests PROPERTIES COMPILE_DEFINITIONS "${POCKET_TEST_DEFINITIONS}")
    target_sources(PocketHostTests PRIVATE Tests/HostTests.cpp)
    target_link_libraries(PocketHostTests PRIVATE PocketBalance juce::juce_audio_utils)
    target_include_directories(PocketHostTests PRIVATE Source)
    add_test(NAME PocketHostTests COMMAND PocketHostTests)
  endif()
endif()
