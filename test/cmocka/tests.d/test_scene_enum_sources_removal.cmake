# Scene source enumeration must tolerate removal of a later scene item from
# the public enumeration callback. The isolated child cases use POSIX fork.
if(NOT WIN32)
  add_executable(test_scene_enum_sources_removal test_scene_enum_sources_removal.c)
  target_include_directories(test_scene_enum_sources_removal PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_link_libraries(test_scene_enum_sources_removal PRIVATE OBS::libobs ${CMOCKA_LIBRARIES})
  add_dependencies(cmocka-tests test_scene_enum_sources_removal)
  add_test(test_scene_enum_sources_removal ${CMAKE_CURRENT_BINARY_DIR}/test_scene_enum_sources_removal)
  set_tests_properties(test_scene_enum_sources_removal PROPERTIES TIMEOUT 90)
endif()
