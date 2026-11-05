# Post-build: copy the native shader pack next to ng2.exe when it has been generated. The pack is
# game-derived and never committed or shipped; without one here, ng2 builds it on first launch.
if(EXISTS "${PACK}")
  execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PACK}" "${DEST}")
else()
  message(STATUS "No native shader pack at ${PACK}: ng2 builds it on first launch")
endif()
