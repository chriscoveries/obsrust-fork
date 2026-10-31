if(OS_MACOS)
  add_executable(test_vt_rate_limit_status test_vt_rate_limit_status.c)
  target_link_libraries(
    test_vt_rate_limit_status
    PRIVATE
      OBS::libobs
      "-framework CoreFoundation"
      "-framework CoreMedia"
      "-framework CoreVideo"
      "-framework VideoToolbox"
  )
  target_compile_options(test_vt_rate_limit_status PRIVATE -fblocks)
  target_compile_definitions(test_vt_rate_limit_status PRIVATE $<$<BOOL:${ENABLE_HEVC}>:ENABLE_HEVC>)
  add_dependencies(cmocka-tests test_vt_rate_limit_status)
  add_test(NAME test_vt_rate_limit_status COMMAND test_vt_rate_limit_status)
endif()
