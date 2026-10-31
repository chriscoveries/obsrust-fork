if(OS_MACOS)
  add_executable(test_vt_rate_state test_vt_rate_state.c)
  target_link_libraries(
    test_vt_rate_state
    PRIVATE
      OBS::libobs
      "-framework CoreFoundation"
      "-framework CoreMedia"
      "-framework CoreVideo"
      "-framework VideoToolbox"
  )
  target_compile_options(test_vt_rate_state PRIVATE -fblocks)
  target_compile_definitions(test_vt_rate_state PRIVATE $<$<BOOL:${ENABLE_HEVC}>:ENABLE_HEVC>)
  add_dependencies(cmocka-tests test_vt_rate_state)
  add_test(NAME test_vt_rate_state COMMAND test_vt_rate_state)
endif()
