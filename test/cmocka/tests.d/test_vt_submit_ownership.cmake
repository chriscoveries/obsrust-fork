if(OS_MACOS)
  add_executable(test_vt_submit_ownership test_vt_submit_ownership.c)
  target_link_libraries(
    test_vt_submit_ownership
    PRIVATE
      OBS::libobs
      "-framework CoreFoundation"
      "-framework CoreMedia"
      "-framework CoreVideo"
      "-framework VideoToolbox"
  )
  target_compile_options(test_vt_submit_ownership PRIVATE -fblocks)
  target_compile_definitions(test_vt_submit_ownership PRIVATE $<$<BOOL:${ENABLE_HEVC}>:ENABLE_HEVC>)
  add_dependencies(cmocka-tests test_vt_submit_ownership)
  add_test(NAME test_vt_submit_ownership COMMAND test_vt_submit_ownership)
endif()
