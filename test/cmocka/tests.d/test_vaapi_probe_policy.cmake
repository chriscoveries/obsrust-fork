# obs-ffmpeg's VAAPI encoder probe: decode-only drivers, one probe per device
# (obs-rust/obs-studio#1168). No libva or GPU is needed.
if(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  add_executable(
    test_vaapi_probe_policy
    test_vaapi_probe_policy.c
    "${CMAKE_SOURCE_DIR}/plugins/obs-ffmpeg/vaapi-probe-policy.c.h"
  )
  target_include_directories(
    test_vaapi_probe_policy
    PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/plugins/obs-ffmpeg"
  )
  target_link_libraries(test_vaapi_probe_policy PRIVATE OBS::libobs ${CMOCKA_LIBRARIES})
  add_dependencies(cmocka-tests test_vaapi_probe_policy)
  add_test(test_vaapi_probe_policy ${CMAKE_CURRENT_BINARY_DIR}/test_vaapi_probe_policy)
endif()
