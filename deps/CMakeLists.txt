# Vendored libraries (specification section 4.4.3). Every target is mwb::<name>; include
# directories are SYSTEM and warnings are switched off so they never interact with -Werror.
# Sources here are never edited by hand: see deps/README.md and deps/update_deps.py.

set(MWB_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(mwb_vendored_interface name)
  add_library(mwb_dep_${name} INTERFACE)
  add_library(mwb::${name} ALIAS mwb_dep_${name})
  target_include_directories(mwb_dep_${name} SYSTEM INTERFACE ${ARGN})
endfunction()

function(mwb_vendored_static name)
  cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDE;DEFINITIONS;LINK" ${ARGN})
  add_library(mwb_dep_${name} STATIC ${ARG_SOURCES})
  add_library(mwb::${name} ALIAS mwb_dep_${name})
  target_include_directories(mwb_dep_${name} SYSTEM PUBLIC ${ARG_INCLUDE})
  target_compile_options(mwb_dep_${name} PRIVATE -w)
  if(ARG_DEFINITIONS)
    target_compile_definitions(mwb_dep_${name} PUBLIC ${ARG_DEFINITIONS})
  endif()
  if(ARG_LINK)
    target_link_libraries(mwb_dep_${name} PUBLIC ${ARG_LINK})
  endif()
  set_target_properties(mwb_dep_${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

# --- SQLite amalgamation with the options the specification requires (section 22.5) ----------
mwb_vendored_static(sqlite
  SOURCES ${MWB_DEPS_DIR}/sqlite/sqlite3.c
  INCLUDE ${MWB_DEPS_DIR}/sqlite)
target_compile_definitions(mwb_dep_sqlite PUBLIC
  SQLITE_ENABLE_FTS5=1
  SQLITE_OMIT_LOAD_EXTENSION=1
  SQLITE_DEFAULT_FOREIGN_KEYS=1
  SQLITE_THREADSAFE=1
  SQLITE_DQS=0
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_ENABLE_DBSTAT_VTAB=1)

mwb_vendored_static(sqlitecpp
  SOURCES
    ${MWB_DEPS_DIR}/sqlitecpp/src/Backup.cpp
    ${MWB_DEPS_DIR}/sqlitecpp/src/Column.cpp
    ${MWB_DEPS_DIR}/sqlitecpp/src/Database.cpp
    ${MWB_DEPS_DIR}/sqlitecpp/src/Exception.cpp
    ${MWB_DEPS_DIR}/sqlitecpp/src/Savepoint.cpp
    ${MWB_DEPS_DIR}/sqlitecpp/src/Statement.cpp
    ${MWB_DEPS_DIR}/sqlitecpp/src/Transaction.cpp
  INCLUDE ${MWB_DEPS_DIR}/sqlitecpp/include
  LINK mwb::sqlite)

# --- libremidi: SMF reader/writer and WinMM device I/O ------------------------------------------
# client.cpp (the optional convenience client) is not built: it needs ordering on port types
# that this libc++ rejects, and the application uses midi_in/midi_out/observer directly.
set(MWB_LIBREMIDI_SOURCES
  ${MWB_DEPS_DIR}/libremidi/include/libremidi/libremidi.cpp
  ${MWB_DEPS_DIR}/libremidi/include/libremidi/midi_in.cpp
  ${MWB_DEPS_DIR}/libremidi/include/libremidi/midi_out.cpp
  ${MWB_DEPS_DIR}/libremidi/include/libremidi/observer.cpp
  ${MWB_DEPS_DIR}/libremidi/include/libremidi/reader.cpp
  ${MWB_DEPS_DIR}/libremidi/include/libremidi/writer.cpp)
if(WIN32)
  mwb_vendored_static(libremidi
    SOURCES ${MWB_LIBREMIDI_SOURCES}
    INCLUDE ${MWB_DEPS_DIR}/libremidi/include
    DEFINITIONS LIBREMIDI_WINMM=1
    LINK winmm)
else()
  mwb_vendored_static(libremidi
    SOURCES ${MWB_LIBREMIDI_SOURCES}
    INCLUDE ${MWB_DEPS_DIR}/libremidi/include)
endif()

# --- miniaudio: WASAPI output, null backend, resampler, WAV encoder ---------------------------
mwb_vendored_static(miniaudio
  SOURCES ${MWB_DEPS_DIR}/miniaudio/miniaudio.c
  INCLUDE ${MWB_DEPS_DIR}/miniaudio
  DEFINITIONS MA_NO_FLAC=1 MA_NO_MP3=1 MA_NO_ENGINE=1 MA_NO_NODE_GRAPH=1 MA_NO_GENERATION=1)
if(WIN32)
  target_link_libraries(mwb_dep_miniaudio PUBLIC ole32)
endif()

mwb_vendored_static(xxhash
  SOURCES ${MWB_DEPS_DIR}/xxhash/xxhash.c
  INCLUDE ${MWB_DEPS_DIR}/xxhash)

mwb_vendored_static(utf8proc
  SOURCES ${MWB_DEPS_DIR}/utf8proc/utf8proc.c
  INCLUDE ${MWB_DEPS_DIR}/utf8proc
  DEFINITIONS UTF8PROC_STATIC=1)

mwb_vendored_static(miniz
  SOURCES ${MWB_DEPS_DIR}/miniz/miniz.c
  INCLUDE ${MWB_DEPS_DIR}/miniz)

# efsw: only the generic and platform sources this build uses.
file(GLOB MWB_EFSW_COMMON CONFIGURE_DEPENDS ${MWB_DEPS_DIR}/efsw/src/efsw/*.cpp)
list(FILTER MWB_EFSW_COMMON EXCLUDE REGEX "(FSEvents|Inotify|Kqueue|Win32)\\.cpp$")
if(WIN32)
  set(MWB_EFSW_PLATFORM
    ${MWB_DEPS_DIR}/efsw/src/efsw/FileWatcherWin32.cpp
    ${MWB_DEPS_DIR}/efsw/src/efsw/WatcherWin32.cpp
    ${MWB_DEPS_DIR}/efsw/src/efsw/platform/win/FileSystemImpl.cpp
    ${MWB_DEPS_DIR}/efsw/src/efsw/platform/win/SystemImpl.cpp)
else()
  set(MWB_EFSW_PLATFORM
    ${MWB_DEPS_DIR}/efsw/src/efsw/FileWatcherInotify.cpp
    ${MWB_DEPS_DIR}/efsw/src/efsw/WatcherInotify.cpp
    ${MWB_DEPS_DIR}/efsw/src/efsw/platform/posix/FileSystemImpl.cpp
    ${MWB_DEPS_DIR}/efsw/src/efsw/platform/posix/SystemImpl.cpp)
endif()
mwb_vendored_static(efsw
  SOURCES ${MWB_EFSW_COMMON} ${MWB_EFSW_PLATFORM}
  INCLUDE ${MWB_DEPS_DIR}/efsw/include)
target_include_directories(mwb_dep_efsw PRIVATE ${MWB_DEPS_DIR}/efsw/src)

# --- Header-only libraries ---------------------------------------------------------------------
mwb_vendored_interface(picosha2 ${MWB_DEPS_DIR}/picosha2)
mwb_vendored_interface(stduuid ${MWB_DEPS_DIR}/stduuid/include)
mwb_vendored_interface(nlohmann_json ${MWB_DEPS_DIR}/nlohmann_json/single_include)
target_compile_definitions(mwb_dep_nlohmann_json INTERFACE JSON_USE_IMPLICIT_CONVERSIONS=0)
mwb_vendored_interface(cli11 ${MWB_DEPS_DIR}/cli11/include)
mwb_vendored_interface(pegtl ${MWB_DEPS_DIR}/pegtl/include)
mwb_vendored_interface(rapidfuzz ${MWB_DEPS_DIR}/rapidfuzz)
mwb_vendored_interface(pcg ${MWB_DEPS_DIR}/pcg/include)
mwb_vendored_interface(readerwriterqueue ${MWB_DEPS_DIR}/readerwriterqueue)
mwb_vendored_interface(concurrentqueue ${MWB_DEPS_DIR}/concurrentqueue)
mwb_vendored_interface(thread_pool ${MWB_DEPS_DIR}/thread_pool/include)
mwb_vendored_interface(spdlog ${MWB_DEPS_DIR}/spdlog/include)
target_compile_definitions(mwb_dep_spdlog INTERFACE SPDLOG_USE_STD_FORMAT=1)
mwb_vendored_interface(doctest ${MWB_DEPS_DIR}/doctest)
mwb_vendored_interface(nanobench ${MWB_DEPS_DIR}/nanobench/src/include)
mwb_vendored_interface(tinysoundfont ${MWB_DEPS_DIR}/tinysoundfont)
# Only the include/ forwarding directory is exposed: the root holds a windows.h that would
# shadow the Windows SDK header.
mwb_vendored_interface(signalsmith_dsp ${MWB_DEPS_DIR}/signalsmith_dsp/include)

# craigsapp/midifile: independent SMF reader used only by tests as an oracle.
file(GLOB MWB_MIDIFILE_SOURCES CONFIGURE_DEPENDS ${MWB_DEPS_DIR}/midifile/src/*.cpp)
mwb_vendored_static(midifile
  SOURCES ${MWB_MIDIFILE_SOURCES}
  INCLUDE ${MWB_DEPS_DIR}/midifile/include)
