# dynamics SIMD helpers: bit-exact + tolerance parity vs the scalar
# loops they replace (plugins/obs-filters/dynamics-simd.h)
add_executable(test_dynamics_simd test_dynamics_simd.c)
target_include_directories(test_dynamics_simd PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/plugins/obs-filters")
target_link_libraries(test_dynamics_simd PRIVATE OBS::libobs ${CMOCKA_LIBRARIES})

add_dependencies(cmocka-tests test_dynamics_simd)
add_test(test_dynamics_simd ${CMAKE_CURRENT_BINARY_DIR}/test_dynamics_simd)
