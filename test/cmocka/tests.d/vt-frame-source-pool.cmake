# Reuse VideoToolbox callback records after asynchronous completion.
if(APPLE)
  add_executable(test_vt_frame_source_pool test_vt_frame_source_pool.c)
  target_include_directories(
    test_vt_frame_source_pool
    PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/plugins/mac-videotoolbox" "${CMAKE_SOURCE_DIR}/libobs"
  )
  target_link_libraries(
    test_vt_frame_source_pool
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
  add_dependencies(cmocka-tests test_vt_frame_source_pool)
  add_test(test_vt_frame_source_pool ${CMAKE_CURRENT_BINARY_DIR}/test_vt_frame_source_pool)
endif()
