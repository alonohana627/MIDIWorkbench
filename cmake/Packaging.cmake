# CPack packaging (specification section 32.5): a portable ZIP first and a per-user MSI through
# WiX, both from the same install rules. The MSI is built only when the WiX toolset is found.

set(CPACK_PACKAGE_NAME "MIDIWorkbench")
set(CPACK_PACKAGE_VENDOR "MIDI Workbench contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Offline MIDI composition and library workbench")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "MIDI Workbench")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME "MIDIWorkbench-${PROJECT_VERSION}-windows-x64")

set(CPACK_GENERATOR "ZIP")
find_program(MWB_WIX_CANDLE NAMES candle wix)
if(MWB_WIX_CANDLE)
  list(APPEND CPACK_GENERATOR "WIX")
endif()

# MSI: per-user scope by default, no file association claimed, uninstall keeps user data
# (libraries, documents and recovery data live under LocalAppData, outside the install folder).
set(CPACK_WIX_UPGRADE_GUID "6C1D53C8-5A4E-4D2B-9A0B-3E0F1D9C7A21")
set(CPACK_WIX_INSTALL_SCOPE "perUser")
set(CPACK_WIX_PRODUCT_ICON "${PROJECT_SOURCE_DIR}/resources/midiworkbench.ico")
set(CPACK_PACKAGE_EXECUTABLES "MIDIWorkbench" "MIDI Workbench")

include(CPack)
