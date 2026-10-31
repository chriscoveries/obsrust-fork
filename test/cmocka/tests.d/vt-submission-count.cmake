# One measurement slot per accepted raw VideoToolbox frame (the zero-copy harness stopped at half its frame limit).
if(APPLE)
  add_executable(test_vt_submission_count test_vt_submission_count.c)
  target_include_directories(
    test_vt_submission_count
    PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/plugins/mac-videotoolbox" "${CMAKE_SOURCE_DIR}/libobs"
  )
  target_link_libraries(
    test_vt_submission_count
    PRIVATE
      OBS::libobs
      ${CMOCKA_LIBRARIES}
      "-framework AVFoundation"
      "-framework Cocoa"
      "-framework CoreFoundation"
      "-framework CoreMedia"
      "-framework CoreVideo"
      "-framework IOSurface"
      "-framework VideoToolbox"
  )
  add_dependencies(cmocka-tests test_vt_submission_count)
  add_test(test_vt_submission_count ${CMAKE_CURRENT_BINARY_DIR}/test_vt_submission_count)
endif()
