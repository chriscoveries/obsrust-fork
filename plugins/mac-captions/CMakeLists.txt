cmake_minimum_required(VERSION 3.28...3.30)

add_library(mac-captions MODULE)
add_library(OBS::mac-captions ALIAS mac-captions)

set(SPEECH_SHIM "${CMAKE_CURRENT_BINARY_DIR}/libmac-captions-speech.dylib")
find_program(SWIFTC_EXECUTABLE swiftc REQUIRED)
add_custom_command(
  OUTPUT "${SPEECH_SHIM}"
  COMMAND
    "${SWIFTC_EXECUTABLE}" -parse-as-library -swift-version 6 -emit-library -O
    -Xlinker -install_name -Xlinker @rpath/libmac-captions-speech.dylib
    "${CMAKE_CURRENT_SOURCE_DIR}/speech-transcriber.swift" -o "${SPEECH_SHIM}"
  DEPENDS speech-transcriber.swift
  VERBATIM
)
add_custom_target(mac-captions-speech-shim DEPENDS "${SPEECH_SHIM}")

target_sources(
  mac-captions
  PRIVATE
    captions-filter.c
    captions-filter-module.c
    captions-filter.h
    captions-rollup.c
    captions-rollup.h
    plugin-main.c
    speech-transcriber.c
    speech-transcriber.h
)
add_dependencies(mac-captions mac-captions-speech-shim)
target_link_libraries(mac-captions PRIVATE OBS::libobs OBS::caption "${SPEECH_SHIM}")
target_link_options(mac-captions PRIVATE "-Wl,-rpath,@loader_path")

add_custom_command(
  TARGET mac-captions POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SPEECH_SHIM}" "$<TARGET_FILE_DIR:mac-captions>"
  VERBATIM
)

set_target_properties_obs(
  mac-captions
  PROPERTIES FOLDER plugins PREFIX ""
             XCODE_ATTRIBUTE_CLANG_WARN_SUSPICIOUS_IMPLICIT_CONVERSION YES
             XCODE_ATTRIBUTE_GCC_WARN_SHADOW YES
)
