cmake_minimum_required(VERSION 3.28)
project(MIDIWorkbench VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # No C++ modules in v1 (section 4.3)

option(MWB_WERROR "Treat warnings in owned code as errors" OFF)
option(MWB_BUILD_GUI "Build the Qt desktop application" ON)
option(MWB_REQUIRE_GUI "Fail configuration when Qt is missing (CI presets)" OFF)
option(MWB_BUILD_TESTS "Build doctest suites" ON)
option(MWB_BUILD_FUZZERS "Build libFuzzer targets (fuzz preset)" OFF)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "The application requires Clang and libc++ (specification section 4.2)")
endif()

include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
  #include <version>
  #include <expected>
  #include <span>
  #include <ranges>
  #include <format>
  #include <thread>
  #include <stop_token>
  #include <atomic>
  #ifndef _LIBCPP_VERSION
  #error LLVM libc++ is required
  #endif
  int main() {
    std::expected<int,int> v = 1;
    std::jthread worker([](std::stop_token) {});
    std::atomic<int> counter{0};
    auto text = std::format(\"{}\", *v);
    return *v - 1 + counter.load() + static_cast<int>(text.size()) - 1;
  }
" MWB_HAS_REQUIRED_STDLIB)
if(NOT MWB_HAS_REQUIRED_STDLIB)
  message(FATAL_ERROR "Required libc++ / C++23 support is missing")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(MwbModule)

add_subdirectory(deps)

if(MWB_BUILD_GUI OR MWB_REQUIRE_GUI)
  if(MWB_REQUIRE_GUI)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Test)
  else()
    find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Widgets Test)
  endif()
  if(NOT Qt6_FOUND)
    message(WARNING "Qt 6 not found: the desktop application and GUI tests are skipped")
    set(MWB_BUILD_GUI OFF)
  else()
    set(MWB_BUILD_GUI ON)
  endif()
endif()

# Build identity reported by `mwb-cli capabilities` and the About box.
string(TIMESTAMP MWB_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
set(MWB_QT_VERSION "${Qt6_VERSION}")
if(NOT MWB_QT_VERSION)
  set(MWB_QT_VERSION "not built")
endif()
configure_file(cmake/build_info.hpp.in "${PROJECT_BINARY_DIR}/generated/mwb/build_info.hpp" @ONLY)

# --- Modules (section 19.1). MODULES lists the only project modules a module may include. ---
mwb_module(core
  LIBRARIES mwb::stduuid mwb::picosha2 mwb::nlohmann_json mwb::xxhash mwb::pcg mwb::utf8proc)
target_include_directories(mwb_core PUBLIC "${PROJECT_BINARY_DIR}/generated")
mwb_module(midi MODULES core LIBRARIES mwb::libremidi)
mwb_module(music MODULES core)
mwb_module(document MODULES core midi LIBRARIES mwb::sqlitecpp)
mwb_module(search MODULES core music LIBRARIES mwb::pegtl mwb::sqlitecpp)
mwb_module(library MODULES core midi music document search LIBRARIES mwb::sqlitecpp mwb::miniz)
mwb_module(playback MODULES core)
mwb_module(audio MODULES core playback
  LIBRARIES mwb::miniaudio mwb::tinysoundfont mwb::signalsmith_dsp mwb::pcg)
mwb_module(jobs MODULES core LIBRARIES mwb::thread_pool mwb::concurrentqueue)
set(MWB_PLATFORM_SYSTEM_LIBRARIES "")
if(WIN32)
  set(MWB_PLATFORM_SYSTEM_LIBRARIES winmm)
endif()
mwb_module(platform MODULES core audio playback
  LIBRARIES mwb::libremidi mwb::miniaudio mwb::efsw mwb::readerwriterqueue mwb::concurrentqueue
            ${MWB_PLATFORM_SYSTEM_LIBRARIES})
mwb_module(worker MODULES core midi music document)
mwb_module(app MODULES core midi music document library search playback audio platform jobs worker
  LIBRARIES mwb::spdlog mwb::sqlite)
include(ThirdPartyNotices)
mwb_generate_third_party_notices("${PROJECT_BINARY_DIR}/generated/mwb_third_party_notices.cpp")
target_sources(mwb_app PRIVATE "${PROJECT_BINARY_DIR}/generated/mwb_third_party_notices.cpp")
if(WIN32)
  target_link_libraries(mwb_app PUBLIC shell32 ole32)
endif()
mwb_module(cli MODULES core midi music document library search playback audio platform app
  LIBRARIES mwb::cli11)


# --- Executables -------------------------------------------------------------------------------
if(EXISTS "${PROJECT_SOURCE_DIR}/src/cli/main/mwb_cli_main.cpp")
  add_executable(mwb-cli src/cli/main/mwb_cli_main.cpp)
  mwb_apply_owned_options(mwb-cli)
  target_link_libraries(mwb-cli PRIVATE mwb::cli)
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/src/worker/main/mwb_worker_main.cpp")
  add_executable(mwb-worker src/worker/main/mwb_worker_main.cpp)
  mwb_apply_owned_options(mwb-worker)
  target_link_libraries(mwb-worker PRIVATE mwb::worker)
endif()

if(MWB_BUILD_GUI AND EXISTS "${PROJECT_SOURCE_DIR}/src/ui/main/midiworkbench_main.cpp")
  set(CMAKE_AUTORCC ON)
  mwb_module(ui QT MODULES core midi music document library search playback audio platform app
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets mwb::rapidfuzz)
  add_executable(midiworkbench WIN32 src/ui/main/midiworkbench_main.cpp resources/midiworkbench.rc
    resources/resources.qrc)
  mwb_apply_owned_options(midiworkbench)
  target_link_libraries(midiworkbench PRIVATE mwb::ui mwb::cli11)
  set_target_properties(midiworkbench PROPERTIES OUTPUT_NAME "MIDIWorkbench")
endif()

mwb_write_include_graph()

# --- Tests -------------------------------------------------------------------------------------
if(MWB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MWB_BUILD_FUZZERS)
  add_subdirectory(tests/fuzz)
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/cmake/Install.cmake")
  include(cmake/Install.cmake)
endif()

# --- Benchmarks (development only; results are stored as JSON) ---------------------------------
if(MWB_BUILD_TESTS)
  add_executable(mwb_bench_editor benchmarks/editor_bench.cpp)
  mwb_apply_owned_options(mwb_bench_editor)
  target_link_libraries(mwb_bench_editor PRIVATE mwb::core mwb::midi mwb::music mwb::document mwb::nanobench)
endif()
