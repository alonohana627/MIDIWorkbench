# Install rules shared by `cmake --install` and CPack (specification section 32.5): the
# executables, the runtime DLLs they actually import from the toolchain prefix (resolved with
# CMake's runtime dependency set, never by copying a whole environment), Qt via windeployqt
# without network plugins, the GPL-3.0 text, every vendored license from deps/manifest.json,
# and an SBOM.

include(GNUInstallDirs)

set(MWB_INSTALL_TARGETS)
foreach(target mwb-cli mwb-worker midiworkbench)
  if(TARGET ${target})
    list(APPEND MWB_INSTALL_TARGETS ${target})
  endif()
endforeach()

if(WIN32)
  get_filename_component(MWB_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
  install(TARGETS ${MWB_INSTALL_TARGETS}
    RUNTIME_DEPENDENCY_SET mwb_runtime
    RUNTIME DESTINATION .)
  install(RUNTIME_DEPENDENCY_SET mwb_runtime
    DESTINATION .
    DIRECTORIES "${MWB_COMPILER_BIN}"
    PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
    POST_EXCLUDE_REGEXES ".*[Ss]ystem32/.*\\.dll" ".*[Ww]indows/.*\\.dll")
  if(TARGET midiworkbench)
    find_program(MWB_WINDEPLOYQT NAMES windeployqt6 windeployqt HINTS "${MWB_COMPILER_BIN}")
    if(MWB_WINDEPLOYQT)
      install(CODE "
        execute_process(COMMAND \"${MWB_WINDEPLOYQT}\" --no-translations --no-system-d3d-compiler
                        --no-opengl-sw --skip-plugin-types networkinformation,tls,qmltooling,generic
                        --dir \"\${CMAKE_INSTALL_PREFIX}\" \"\${CMAKE_INSTALL_PREFIX}/MIDIWorkbench.exe\"
                        RESULT_VARIABLE mwb_deploy_result)
        if(NOT mwb_deploy_result EQUAL 0)
          message(FATAL_ERROR \"windeployqt failed: \${mwb_deploy_result}\")
        endif()")
      # The offscreen platform plugin enables headless smoke tests of the installed copy
      # (QT_QPA_PLATFORM=offscreen, section 4.5 item 9).
      find_file(MWB_QOFFSCREEN qoffscreen.dll
        HINTS "${MWB_COMPILER_BIN}/../share/qt6/plugins/platforms" NO_DEFAULT_PATH)
      if(MWB_QOFFSCREEN)
        install(FILES "${MWB_QOFFSCREEN}" DESTINATION platforms)
      endif()
    endif()
  endif()
else()
  install(TARGETS ${MWB_INSTALL_TARGETS} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME GPL-3.0.txt)
install(FILES "${PROJECT_SOURCE_DIR}/README.md" DESTINATION .)
file(READ "${PROJECT_SOURCE_DIR}/deps/manifest.json" MWB_MANIFEST)
string(JSON MWB_DEP_COUNT LENGTH "${MWB_MANIFEST}" dependencies)
math(EXPR MWB_DEP_LAST "${MWB_DEP_COUNT} - 1")
set(MWB_SBOM_COMPONENTS "")
foreach(index RANGE ${MWB_DEP_LAST})
  string(JSON dep_name GET "${MWB_MANIFEST}" dependencies ${index} name)
  string(JSON dep_version GET "${MWB_MANIFEST}" dependencies ${index} version)
  string(JSON dep_license GET "${MWB_MANIFEST}" dependencies ${index} license)
  string(JSON dep_license_file GET "${MWB_MANIFEST}" dependencies ${index} license_file)
  install(FILES "${PROJECT_SOURCE_DIR}/deps/${dep_name}/${dep_license_file}"
    DESTINATION licenses/${dep_name})
  string(APPEND MWB_SBOM_COMPONENTS
    "    {\"type\": \"library\", \"name\": \"${dep_name}\", \"version\": \"${dep_version}\", \"licenses\": [{\"expression\": \"${dep_license}\"}]},\n")
endforeach()
string(APPEND MWB_SBOM_COMPONENTS
  "    {\"type\": \"framework\", \"name\": \"Qt\", \"version\": \"${MWB_QT_VERSION}\", \"licenses\": [{\"expression\": \"LGPL-3.0-only\"}]}\n")
file(WRITE "${PROJECT_BINARY_DIR}/sbom.cdx.json"
  "{\n  \"bomFormat\": \"CycloneDX\",\n  \"specVersion\": \"1.5\",\n  \"metadata\": {\"component\": {\"type\": \"application\", \"name\": \"MIDI Workbench\", \"version\": \"${PROJECT_VERSION}\", \"licenses\": [{\"expression\": \"GPL-3.0-only\"}]}},\n  \"components\": [\n${MWB_SBOM_COMPONENTS}  ]\n}\n")
install(FILES "${PROJECT_BINARY_DIR}/sbom.cdx.json" DESTINATION licenses)

include("${PROJECT_SOURCE_DIR}/cmake/Packaging.cmake")
