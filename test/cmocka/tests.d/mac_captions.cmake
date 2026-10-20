if(OS_MACOS)
  set(MAC_CAPTIONS_SPEECH_SHIM "${CMAKE_CURRENT_BINARY_DIR}/libmac-captions-speech-test.dylib")
  set(MAC_CAPTIONS_ASSET_INSTALLER "${CMAKE_CURRENT_BINARY_DIR}/install-speech-assets")
  find_program(SWIFTC_EXECUTABLE swiftc REQUIRED)
  add_custom_command(
    OUTPUT "${MAC_CAPTIONS_SPEECH_SHIM}"
    COMMAND
      "${SWIFTC_EXECUTABLE}" -parse-as-library -swift-version 6 -emit-library -O
      -Xlinker -install_name -Xlinker @rpath/libmac-captions-speech-test.dylib
      "${CMAKE_SOURCE_DIR}/plugins/mac-captions/speech-transcriber.swift"
      -o "${MAC_CAPTIONS_SPEECH_SHIM}"
    DEPENDS "${CMAKE_SOURCE_DIR}/plugins/mac-captions/speech-transcriber.swift"
    VERBATIM
  )
  add_custom_target(mac-captions-speech-test-shim DEPENDS "${MAC_CAPTIONS_SPEECH_SHIM}")
  # This standalone tool installs assets only when explicitly invoked with its
  # opt-in flag. The filter runtime never calls the asset installation API.
  add_custom_command(
    OUTPUT "${MAC_CAPTIONS_ASSET_INSTALLER}"
    COMMAND
      "${SWIFTC_EXECUTABLE}" -parse-as-library -swift-version 6
      -D MAC_CAPTIONS_ASSET_INSTALLER
      "${CMAKE_SOURCE_DIR}/plugins/mac-captions/speech-transcriber.swift"
      -o "${MAC_CAPTIONS_ASSET_INSTALLER}"
    DEPENDS "${CMAKE_SOURCE_DIR}/plugins/mac-captions/speech-transcriber.swift"
    VERBATIM
  )
  add_custom_target(mac-captions-install-speech-assets DEPENDS "${MAC_CAPTIONS_ASSET_INSTALLER}")

  add_executable(
    test_mac_captions
    test_mac_captions.c
    "${CMAKE_SOURCE_DIR}/plugins/mac-captions/captions-filter.c"
    "${CMAKE_SOURCE_DIR}/plugins/mac-captions/captions-filter-module.c"
    "${CMAKE_SOURCE_DIR}/plugins/mac-captions/captions-rollup.c"
    "${CMAKE_SOURCE_DIR}/plugins/mac-captions/speech-transcriber.c"
  )
  add_dependencies(test_mac_captions mac-captions-speech-test-shim)
  target_include_directories(
    test_mac_captions
    PRIVATE
      ${CMOCKA_INCLUDE_DIR}
      "${CMAKE_CURRENT_SOURCE_DIR}"
      "${CMAKE_SOURCE_DIR}/libobs"
      "${CMAKE_SOURCE_DIR}/deps/libcaption"
      "${CMAKE_SOURCE_DIR}/plugins/mac-captions"
  )
  target_compile_definitions(
    test_mac_captions
    PRIVATE
      MAC_CAPTIONS_AUDIO_FIXTURE="${CMAKE_SOURCE_DIR}/test/cmocka/data/live-captions.wav"
      MAC_CAPTIONS_WORDS_FIXTURE="${CMAKE_SOURCE_DIR}/test/cmocka/data/live-captions.words"
      MAC_CAPTIONS_SPEECH_FIXTURE="${CMAKE_SOURCE_DIR}/test/cmocka/data/live-captions-speech.wav"
      MAC_CAPTIONS_REFERENCE_FIXTURE="${CMAKE_SOURCE_DIR}/test/cmocka/data/live-captions-reference.txt"
  )
  target_link_libraries(test_mac_captions PRIVATE OBS::libobs OBS::caption "${MAC_CAPTIONS_SPEECH_SHIM}" ${CMOCKA_LIBRARIES})
  target_link_options(test_mac_captions PRIVATE "-Wl,-rpath,@loader_path")
  add_custom_command(
    TARGET test_mac_captions POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_directory "$<TARGET_FILE_DIR:libobs>/../.."
      "$<TARGET_FILE_DIR:test_mac_captions>/../Frameworks/libobs.framework"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MAC_CAPTIONS_SPEECH_SHIM}" "$<TARGET_FILE_DIR:test_mac_captions>"
    VERBATIM
  )
  add_dependencies(cmocka-tests test_mac_captions)
  add_test(test_mac_captions ${CMAKE_CURRENT_BINARY_DIR}/test_mac_captions)
  get_filename_component(MAC_CAPTIONS_FFMPEG_LIB_DIR "${FFmpeg_avcodec_LIBRARY}" DIRECTORY)
  set_tests_properties(
    test_mac_captions
    PROPERTIES
      ENVIRONMENT "DYLD_LIBRARY_PATH=${MAC_CAPTIONS_FFMPEG_LIB_DIR}"
  )
endif()
