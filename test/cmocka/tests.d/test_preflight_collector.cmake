# Exercise the OBS stats sampler together with the real Rust decision engine.
if((OS_LINUX OR OS_MACOS) AND TARGET OBS::libobs-opengl AND TARGET obs-preflight-rust)
  if(OS_LINUX)
    find_program(XVFB_RUN_EXECUTABLE xvfb-run REQUIRED)
  endif()

  set(
    _preflight_archive
    "${CMAKE_BINARY_DIR}/obs-preflight-cargo/release/${CMAKE_STATIC_LIBRARY_PREFIX}obs_preflight_ffi${CMAKE_STATIC_LIBRARY_SUFFIX}"
  )
  add_executable(
    test_preflight_collector
    test_preflight_collector.c
    "${CMAKE_SOURCE_DIR}/frontend/preflight/obs-preflight-collector.c"
  )
  target_compile_definitions(test_preflight_collector PRIVATE GRAPHICS_MODULE_PATH="$<TARGET_FILE:OBS::libobs-opengl>")
  target_include_directories(
    test_preflight_collector
    PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/frontend/preflight"
  )
  target_link_libraries(
    test_preflight_collector
    PRIVATE OBS::libobs "${_preflight_archive}" Threads::Threads ${CMAKE_DL_LIBS} ${CMOCKA_LIBRARIES}
  )
  if(NOT MSVC)
    target_link_libraries(test_preflight_collector PRIVATE m)
  endif()
  add_dependencies(test_preflight_collector libobs-opengl obs-preflight-rust)
  add_dependencies(cmocka-tests test_preflight_collector)

  if(OS_LINUX)
    add_test(
      NAME test_preflight_collector
      COMMAND "${XVFB_RUN_EXECUTABLE}" -a "$<TARGET_FILE:test_preflight_collector>"
      WORKING_DIRECTORY "${OBS_OUTPUT_DIR}/$<CONFIG>"
    )
  else()
    add_test(
      NAME test_preflight_collector
      COMMAND "$<TARGET_FILE:test_preflight_collector>"
      WORKING_DIRECTORY "$<TARGET_FILE_DIR:test_preflight_collector>"
    )
  endif()
  set_tests_properties(test_preflight_collector PROPERTIES TIMEOUT 60)
endif()
