if(APPLE)
  add_executable(
    test_metal_texture_completion
    test_metal_texture_completion.swift
    "${CMAKE_SOURCE_DIR}/libobs-metal/MetalTextureCompletion.swift"
  )
  target_link_libraries(test_metal_texture_completion PRIVATE "-framework Metal")
  target_link_options(test_metal_texture_completion PRIVATE "LINKER:-headerpad_max_install_names")
  set_target_properties(test_metal_texture_completion PROPERTIES XCODE_ATTRIBUTE_SWIFT_VERSION 6.0)
  add_dependencies(cmocka-tests test_metal_texture_completion)
  add_test(test_metal_texture_completion ${CMAKE_CURRENT_BINARY_DIR}/test_metal_texture_completion)
  set_tests_properties(test_metal_texture_completion PROPERTIES SKIP_RETURN_CODE 77)
endif()
