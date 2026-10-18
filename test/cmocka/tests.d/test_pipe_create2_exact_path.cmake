if(UNIX AND TARGET OBS::libobs)
  add_executable(test_pipe_create2_exact_path test_pipe_create2_exact_path.c)
  target_include_directories(test_pipe_create2_exact_path PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_link_libraries(test_pipe_create2_exact_path PRIVATE OBS::libobs ${CMOCKA_LIBRARIES})

  add_dependencies(cmocka-tests test_pipe_create2_exact_path)
  add_test(NAME test_pipe_create2_exact_path COMMAND test_pipe_create2_exact_path)
  set_tests_properties(test_pipe_create2_exact_path PROPERTIES TIMEOUT 20)
endif()
