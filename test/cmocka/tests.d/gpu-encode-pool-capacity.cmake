# The async IOSurface pool must cover VideoToolbox's retained frames before it
# falls back to repeating a queued frame.
add_executable(test_gpu_encode_pool_capacity test_gpu_encode_pool_capacity.c)
target_include_directories(test_gpu_encode_pool_capacity PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/deps/libcaption")
target_link_libraries(test_gpu_encode_pool_capacity PRIVATE OBS::libobs ${CMOCKA_LIBRARIES})
add_dependencies(cmocka-tests test_gpu_encode_pool_capacity)
add_test(test_gpu_encode_pool_capacity ${CMAKE_CURRENT_BINARY_DIR}/test_gpu_encode_pool_capacity)
