# Bytes-copied baseline for the Mac zero-copy epic (fork #47). macOS only:
# drives a real CVPixelBuffer with no OBS session.
if(APPLE)
  add_executable(test_vt_frame_copy test_vt_frame_copy.c "${CMAKE_SOURCE_DIR}/plugins/mac-videotoolbox/vt-frame-copy.c")
  target_include_directories(
    test_vt_frame_copy
    PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/plugins/mac-videotoolbox" "${CMAKE_SOURCE_DIR}/libobs"
  )
  target_link_libraries(
    test_vt_frame_copy
    PRIVATE
      OBS::libobs
      ${CMOCKA_LIBRARIES}
      "-framework VideoToolbox"
      "-framework CoreVideo"
      "-framework CoreMedia"
      "-framework Foundation"
  )
  add_dependencies(cmocka-tests test_vt_frame_copy)
  add_test(test_vt_frame_copy ${CMAKE_CURRENT_BINARY_DIR}/test_vt_frame_copy)
endif()
