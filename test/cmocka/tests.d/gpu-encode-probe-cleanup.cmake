# The async sample-probe cleanup ordering is shared with start_gpu_encode() and
# can be exercised without a renderer or display.
add_executable(test_gpu_encode_probe_cleanup test_gpu_encode_probe_cleanup.c)
target_include_directories(test_gpu_encode_probe_cleanup PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_gpu_encode_probe_cleanup PRIVATE ${CMOCKA_LIBRARIES} Threads::Threads)
add_dependencies(cmocka-tests test_gpu_encode_probe_cleanup)
add_test(test_gpu_encode_probe_cleanup ${CMAKE_CURRENT_BINARY_DIR}/test_gpu_encode_probe_cleanup)
