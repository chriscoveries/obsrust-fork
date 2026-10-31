# Fault-injected VideoToolbox drain and production raw-fallback validation.
if(APPLE)
  add_executable(test_vt_texture_raw_fallback test_vt_texture_raw_fallback.c)
  target_include_directories(
    test_vt_texture_raw_fallback
    PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/plugins/mac-videotoolbox" "${CMAKE_SOURCE_DIR}/libobs"
  )
  target_link_libraries(
    test_vt_texture_raw_fallback
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
  add_dependencies(cmocka-tests test_vt_texture_raw_fallback)
  add_test(test_vt_texture_raw_fallback ${CMAKE_CURRENT_BINARY_DIR}/test_vt_texture_raw_fallback)
endif()
