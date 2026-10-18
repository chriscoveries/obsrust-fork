if(TARGET OBS::libobs)
  add_executable(test_config_hex_float_rounding test_config_hex_float_rounding.c)
  target_include_directories(test_config_hex_float_rounding PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_link_libraries(test_config_hex_float_rounding PRIVATE OBS::libobs ${CMOCKA_LIBRARIES})

  add_dependencies(cmocka-tests test_config_hex_float_rounding)
  add_test(NAME test_config_hex_float_rounding COMMAND test_config_hex_float_rounding)
  set_tests_properties(test_config_hex_float_rounding PROPERTIES TIMEOUT 20)
endif()
