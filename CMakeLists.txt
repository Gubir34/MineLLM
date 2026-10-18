cmake_minimum_required(VERSION 3.16)
project(minellm VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The MineMCP core (git submodule): builds the minemcp daemon MineLLM drives.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/core/CMakeLists.txt)
  message(FATAL_ERROR "core/ is empty: run `git submodule update --init`")
endif()
add_subdirectory(core EXCLUDE_FROM_ALL)
set_target_properties(minemcp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
  gtk4>=4.14 libadwaita-1>=1.6 json-glib-1.0 libsoup-3.0 libsecret-1)

find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/data/minellm.gresource.xml)
set(GRESOURCE_C ${CMAKE_BINARY_DIR}/resources.c)
add_custom_command(
  OUTPUT ${GRESOURCE_C}
  COMMAND ${GLIB_COMPILE_RESOURCES} --target=${GRESOURCE_C} --generate-source
          --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/data ${GRESOURCE_XML}
  DEPENDS ${GRESOURCE_XML} data/tools.json data/style.css
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(minellm
  src/main.c
  src/window.c
  src/agent.c
  src/llm.c
  src/core_client.c
  src/settings.c
  ${GRESOURCE_C})
target_compile_definitions(minellm PRIVATE MINELLM_VERSION="${PROJECT_VERSION}")
target_compile_options(minellm PRIVATE -Wall -Wextra -Wshadow -Wformat=2)
target_link_libraries(minellm PRIVATE PkgConfig::DEPS)
add_dependencies(minellm minemcp)

include(GNUInstallDirs)
install(TARGETS minellm minemcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/io.github.gubir34.MineLLM.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/icons/io.github.gubir34.MineLLM.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

# Developer tool: the agent without the window (not installed).
add_executable(agent_probe EXCLUDE_FROM_ALL tests/agent_probe.c src/agent.c src/llm.c src/core_client.c ${GRESOURCE_C})
target_link_libraries(agent_probe PRIVATE PkgConfig::DEPS)
add_dependencies(agent_probe minemcp)
add_executable(window_shot EXCLUDE_FROM_ALL tests/window_shot.c src/window.c src/agent.c src/llm.c src/core_client.c
               src/settings.c ${GRESOURCE_C})
target_compile_definitions(window_shot PRIVATE MINELLM_VERSION="${PROJECT_VERSION}")
target_link_libraries(window_shot PRIVATE PkgConfig::DEPS)
