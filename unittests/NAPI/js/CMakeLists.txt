add_custom_command(
  OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/node_modules.md5
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/yarn.lock
  COMMAND yarn install --frozen-lockfile
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
 
add_custom_target(installNapiTestJsTools
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/node_modules.md5
)