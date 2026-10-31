# Reuse one CoreVideo wrapper for each IOSurface-backed texture.
if(APPLE)
  add_executable(test_vt_texture_pixel_buffer_cache test_vt_texture_pixel_buffer_cache.c)
  target_include_directories(
    test_vt_texture_pixel_buffer_cache
    PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/plugins/mac-videotoolbox" "${CMAKE_SOURCE_DIR}/libobs"
  )
  target_link_libraries(
    test_vt_texture_pixel_buffer_cache
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
  add_dependencies(cmocka-tests test_vt_texture_pixel_buffer_cache)
  add_test(test_vt_texture_pixel_buffer_cache ${CMAKE_CURRENT_BINARY_DIR}/test_vt_texture_pixel_buffer_cache)
endif()
