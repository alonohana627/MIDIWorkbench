# libFuzzer targets (specification section 34.4): one per untrusted-input entry point. Built by
# the `fuzz` preset (ASan + fuzzer instrumentation). Seeds live in tests/fuzz/seeds/<target>/.
#
# The MSYS2 CLANG64 driver rejects -fsanitize=fuzzer for the windows-gnu target, although the
# libFuzzer runtime ships with it. The preset therefore compiles with ASan plus libFuzzer's
# coverage instrumentation and the targets link libclang_rt.fuzzer explicitly.
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir OUTPUT_VARIABLE MWB_CLANG_RESOURCE_DIR
                OUTPUT_STRIP_TRAILING_WHITESPACE)
find_library(MWB_LIBFUZZER NAMES clang_rt.fuzzer-x86_64 libclang_rt.fuzzer-x86_64.a
             PATHS "${MWB_CLANG_RESOURCE_DIR}/lib/windows" "${MWB_CLANG_RESOURCE_DIR}/lib/linux" NO_DEFAULT_PATH)
if(NOT MWB_LIBFUZZER)
  message(FATAL_ERROR "libFuzzer runtime not found under ${MWB_CLANG_RESOURCE_DIR}")
endif()

function(mwb_fuzz_target name)
  cmake_parse_arguments(ARG "" "" "MODULES" ${ARGN})
  add_executable(mwb_fuzz_${name} ${name}_fuzz.cpp)
  target_link_libraries(mwb_fuzz_${name} PRIVATE "${MWB_LIBFUZZER}")
  foreach(module IN LISTS ARG_MODULES)
    target_link_libraries(mwb_fuzz_${name} PRIVATE mwb::${module})
  endforeach()
endfunction()

mwb_fuzz_target(smf_import MODULES core midi)
mwb_fuzz_target(rmid_unwrap MODULES core midi)
mwb_fuzz_target(native_loader MODULES core midi document)
mwb_fuzz_target(command_codec MODULES core midi document)
mwb_fuzz_target(query_parser MODULES core music search)
mwb_fuzz_target(motif_stream MODULES core music search)
mwb_fuzz_target(ipc_frames MODULES core midi music document worker)
mwb_fuzz_target(soundfont MODULES core playback audio)
mwb_fuzz_target(recipe_json MODULES core midi music)
