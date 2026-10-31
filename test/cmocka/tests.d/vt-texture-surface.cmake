# Plane-level parity check for the Metal/IOSurface VideoToolbox bridge.
if(APPLE)
  add_executable(
    test_vt_texture_surface
    test_vt_texture_surface.m
    "${CMAKE_SOURCE_DIR}/plugins/mac-videotoolbox/vt-frame-copy.c"
  )
  target_include_directories(
    test_vt_texture_surface
    PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/plugins/mac-videotoolbox" "${CMAKE_SOURCE_DIR}/libobs"
  )
  target_link_libraries(
    test_vt_texture_surface
    PRIVATE
      OBS::libobs
      ${CMOCKA_LIBRARIES}
      "-framework CoreMedia"
      "-framework CoreVideo"
      "-framework Foundation"
      "-framework IOSurface"
      "-framework Metal"
      "-framework VideoToolbox"
  )
  add_dependencies(cmocka-tests test_vt_texture_surface)
  add_test(test_vt_texture_surface ${CMAKE_CURRENT_BINARY_DIR}/test_vt_texture_surface)
endif()
