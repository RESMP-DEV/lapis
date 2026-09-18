include_guard(GLOBAL)

set(LAPIS_GHOSTTY_PREFIX "" CACHE PATH "Ghostty VT probe prefix")
if(NOT LAPIS_GHOSTTY_PREFIX AND DEFINED ENV{LAPIS_GHOSTTY_PREFIX})
  set(LAPIS_GHOSTTY_PREFIX "$ENV{LAPIS_GHOSTTY_PREFIX}" CACHE PATH
      "Ghostty VT probe prefix" FORCE)
endif()

if(NOT LAPIS_GHOSTTY_PREFIX)
  message(FATAL_ERROR
    "LAPIS_GHOSTTY_PREFIX is required. Build and validate the pinned Ghostty VT "
    "artifact with: python3 scripts/probe_terminal.py --engine ghostty. Use "
    "the run prefix printed by that command.")
endif()

cmake_path(SET LAPIS_GHOSTTY_PREFIX_NORMAL NORMALIZE "${LAPIS_GHOSTTY_PREFIX}")
set(LAPIS_GHOSTTY_HEADER
  "${LAPIS_GHOSTTY_PREFIX_NORMAL}/include/ghostty/vt.h")
set(LAPIS_GHOSTTY_STATIC_LIBRARY
  "${LAPIS_GHOSTTY_PREFIX_NORMAL}/lib/libghostty-vt.a")
set(LAPIS_GHOSTTY_RECEIPT
  "${LAPIS_GHOSTTY_PREFIX_NORMAL}/../reports/receipt.json")

foreach(artifact IN ITEMS "${LAPIS_GHOSTTY_HEADER}" "${LAPIS_GHOSTTY_STATIC_LIBRARY}" "${LAPIS_GHOSTTY_RECEIPT}")
  if(NOT EXISTS "${artifact}")
    message(FATAL_ERROR
      "Required Ghostty VT artifact does not exist: ${artifact}. Set "
      "LAPIS_GHOSTTY_PREFIX to the probe prefix, or rebuild it with: "
      "python3 scripts/probe_terminal.py --engine ghostty. Use the run "
      "prefix printed by that command.")
  endif()
endforeach()

# Compare normalized JSON using CMake itself; no second dependency runner.
file(READ "${LAPIS_GHOSTTY_RECEIPT}" lapis_ghostty_receipt)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../tools/terminal_probe/ghostty/sources.json"
    lapis_ghostty_manifest)
string(JSON lapis_ghostty_engine GET "${lapis_ghostty_receipt}" engine)
string(JSON lapis_ghostty_passed GET "${lapis_ghostty_receipt}" passed)
string(JSON lapis_ghostty_sources GET "${lapis_ghostty_receipt}" sources)
string(JSON lapis_ghostty_sources_equal EQUAL
    "${lapis_ghostty_sources}" "${lapis_ghostty_manifest}")
if(NOT lapis_ghostty_engine STREQUAL "ghostty" OR
   NOT lapis_ghostty_passed STREQUAL "ON" OR
   NOT lapis_ghostty_sources_equal)
    message(FATAL_ERROR "Ghostty prefix receipt is unsuccessful or differs from pinned sources. "
        "Rebuild with: python3 scripts/probe_terminal.py --engine ghostty")
endif()

add_library(lapis_ghostty_vt STATIC IMPORTED GLOBAL)
target_include_directories(lapis_ghostty_vt INTERFACE
  "${LAPIS_GHOSTTY_PREFIX_NORMAL}/include")
set_target_properties(lapis_ghostty_vt PROPERTIES
  IMPORTED_LOCATION "${LAPIS_GHOSTTY_STATIC_LIBRARY}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(lapis_ghostty_vt INTERFACE m pthread dl)
endif()
