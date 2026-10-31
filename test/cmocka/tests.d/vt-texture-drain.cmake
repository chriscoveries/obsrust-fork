# Fault-injected VideoToolbox drain and production raw-fallback validation.
if(APPLE)
  add_executable(test_vt_texture_drain test_vt_texture_drain.c)
  target_include_directories(
    test_vt_texture_drain
    PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/plugins/mac-videotoolbox" "${CMAKE_SOURCE_DIR}/libobs"
  )
  target_link_libraries(
    test_vt_texture_drain
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
  add_dependencies(cmocka-tests test_vt_texture_drain)
  add_test(test_vt_texture_drain ${CMAKE_CURRENT_BINARY_DIR}/test_vt_texture_drain)
endif()
