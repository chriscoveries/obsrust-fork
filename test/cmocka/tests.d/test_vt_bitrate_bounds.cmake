if(OS_MACOS)
  add_executable(test_vt_bitrate_bounds test_vt_bitrate_bounds.c)
  target_link_libraries(
    test_vt_bitrate_bounds
    PRIVATE
      OBS::libobs
      "-framework CoreFoundation"
      "-framework CoreMedia"
      "-framework CoreVideo"
      "-framework VideoToolbox"
  )
  target_compile_options(test_vt_bitrate_bounds PRIVATE -fblocks)
  target_compile_definitions(test_vt_bitrate_bounds PRIVATE $<$<BOOL:${ENABLE_HEVC}>:ENABLE_HEVC>)
  add_dependencies(cmocka-tests test_vt_bitrate_bounds)
  add_test(NAME test_vt_bitrate_bounds COMMAND test_vt_bitrate_bounds)
endif()
