# Post-build: copy the native shader pack next to ng2.exe when it has been generated. The pack is
# game-derived, built locally by tools/native-shader-tool, and never committed, so a checkout
# without it still builds.
if(EXISTS "${PACK}")
  execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PACK}" "${DEST}")
else()
  message(STATUS "No native shader pack at ${PACK}: the native renderer will stay off")
endif()
