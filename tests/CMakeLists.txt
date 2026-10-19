# Test executables (specification section 34.1). Each tests/<area>_tests.cpp becomes one
# executable; mwb_test_suite registers one CTest entry per doctest TEST_SUITE.

add_library(mwb_test_support STATIC test_support.cpp test_support.hpp)
target_link_libraries(mwb_test_support PUBLIC mwb::doctest)
target_include_directories(mwb_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(mwb_test_support PUBLIC cxx_std_23)

# area -> modules it links. A suite is registered once its source file exists.
set(MWB_TEST_AREAS
  "core|core"
  "midi|core midi"
  "music|core music"
  "document|core midi document"
  "search|core music search"
  "library|core midi music document search library"
  "playback|core playback"
  "audio|core playback audio"
  "platform|core audio playback platform"
  "jobs|core jobs"
  "worker|core midi music document worker"
  "app|core midi music document library search playback audio platform jobs worker app"
  "cli|core midi music document library search playback audio platform app cli")
foreach(entry IN LISTS MWB_TEST_AREAS)
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 area)
  list(GET parts 1 modules)
  string(REPLACE " " ";" modules "${modules}")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${area}_tests.cpp")
    mwb_test_suite(${area} MODULES ${modules})
  endif()
endforeach()
foreach(tests IN ITEMS mwb_worker_tests mwb_app_tests)
  if(TARGET ${tests} AND TARGET mwb-worker)
    target_compile_definitions(${tests} PRIVATE MWB_WORKER_PATH="$<TARGET_FILE:mwb-worker>")
    add_dependencies(${tests} mwb-worker)
  endif()
endforeach()
if(TARGET mwb_midi_tests)
  target_link_libraries(mwb_midi_tests PRIVATE mwb::midifile)
endif()
if(MWB_BUILD_GUI AND TARGET mwb_ui AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ui_tests.cpp")
  mwb_test_suite(ui QT MODULES core midi music document library search playback audio platform app ui
    LIBRARIES Qt6::Test)
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME include-graph
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check_include_graph.py"
            --graph "${PROJECT_BINARY_DIR}/include_graph.json" --root "${PROJECT_SOURCE_DIR}")
endif()
