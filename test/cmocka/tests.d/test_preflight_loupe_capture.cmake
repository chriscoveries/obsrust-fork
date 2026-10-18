# Exercise the production loupe capture worker with packets from real OBS H.264
# encoders. Core video requires graphics; Linux runs through Xvfb like the
# headless collector test, while macOS uses libobs OpenGL directly.
if((OS_LINUX OR OS_MACOS) AND TARGET OBS::libobs-opengl AND TARGET obs-x264)
  if(OS_LINUX)
    find_program(XVFB_RUN_EXECUTABLE xvfb-run REQUIRED)
  endif()

  add_executable(
    test_preflight_loupe_capture
    test_preflight_loupe_capture.cpp
    "${CMAKE_SOURCE_DIR}/frontend/preflight/preflight-loupe-capture.cpp"
    "${CMAKE_SOURCE_DIR}/frontend/preflight/preflight-loupe-core.cpp"
  )
  target_compile_definitions(
    test_preflight_loupe_capture
    PRIVATE
      GRAPHICS_MODULE_PATH="$<TARGET_FILE:OBS::libobs-opengl>"
      OBS_X264_MODULE_PATH="$<TARGET_FILE:obs-x264>"
      OBS_X264_DATA_PATH="${CMAKE_SOURCE_DIR}/plugins/obs-x264/data"
  )
  target_include_directories(
    test_preflight_loupe_capture
    PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/frontend/preflight"
  )
  target_link_libraries(
    test_preflight_loupe_capture
    PRIVATE OBS::libobs FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale Threads::Threads ${CMOCKA_LIBRARIES}
  )
  if(OS_MACOS)
    # FFmpeg's dylibs use @rpath names; unlike OBS.app, this test executable is
    # not bundled with those dylibs under Frameworks. Use the deps directory as
    # a runpath for the build executable so hardened-runtime macOS still finds it.
    get_filename_component(_preflight_ffmpeg_lib_dir "${FFmpeg_avcodec_LIBRARY}" DIRECTORY)
    target_link_options(
      test_preflight_loupe_capture
      PRIVATE "LINKER:-rpath,${_preflight_ffmpeg_lib_dir}" "LINKER:-rpath,$<TARGET_FILE_DIR:OBS::libobs>/../../.."
    )
    # OBS's app targets use hardened runtime signing. This test executable is
    # ad-hoc signed and loads the separately signed prebuilt FFmpeg dylibs.
    set_property(TARGET test_preflight_loupe_capture PROPERTY XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME NO)
  endif()
  add_dependencies(test_preflight_loupe_capture libobs-opengl obs-x264)

  if(OS_MACOS AND TARGET mac-videotoolbox)
    target_compile_definitions(
      test_preflight_loupe_capture
      PRIVATE
        OBS_VIDEOTOOLBOX_MODULE_PATH="$<TARGET_FILE:mac-videotoolbox>"
        OBS_VIDEOTOOLBOX_DATA_PATH="${CMAKE_SOURCE_DIR}/plugins/mac-videotoolbox/data"
    )
    add_dependencies(test_preflight_loupe_capture mac-videotoolbox)
  endif()

  add_dependencies(cmocka-tests test_preflight_loupe_capture)
  if(OS_LINUX)
    add_test(
      NAME test_preflight_loupe_capture
      COMMAND "${XVFB_RUN_EXECUTABLE}" -a "$<TARGET_FILE:test_preflight_loupe_capture>"
      WORKING_DIRECTORY "${OBS_OUTPUT_DIR}/$<CONFIG>"
    )
  else()
    add_test(
      NAME test_preflight_loupe_capture
      COMMAND
        "${CMAKE_COMMAND}" -E env "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:FFmpeg::avcodec>"
        "$<TARGET_FILE:test_preflight_loupe_capture>"
      WORKING_DIRECTORY "$<TARGET_FILE_DIR:test_preflight_loupe_capture>"
    )
  endif()
  set_tests_properties(test_preflight_loupe_capture PROPERTIES TIMEOUT 180)
endif()
