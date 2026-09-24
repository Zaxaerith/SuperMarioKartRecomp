cmake_minimum_required(VERSION 3.20)
project(SuperMarioKartRecomp C)

set(CMAKE_C_STANDARD 11)

# Build options
option(SMK_BUILD_PLAY "Build interactive desktop player (SDL2)" ON)
option(SMK_BUILD_HEADLESS "Build headless diagnostic test host" ON)

# snesrecomp runner framework
set(SNESRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/snesrecomp")
include(${SNESRECOMP_ROOT}/runner/runner.cmake)

# Recompiled Super Mario Kart game code (statically generated C translation units)
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/game/*.c")

add_library(snesrecomp_game STATIC ${GAME_SOURCES})
target_include_directories(snesrecomp_game PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/config"
  "${CMAKE_CURRENT_SOURCE_DIR}/snesrecomp/runner/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/snesrecomp/runner/src/snes")

if(NOT MSVC)
  target_compile_options(snesrecomp_game PRIVATE
    -w -Wno-implicit-function-declaration)
else()
  target_compile_options(snesrecomp_game PRIVATE /w)
endif()

# Locate SDL2 for windowing, controller input and audio playback
if(SMK_BUILD_PLAY OR SMK_BUILD_HEADLESS)
  find_package(SDL2 CONFIG QUIET)
  if(NOT SDL2_FOUND)
    find_package(SDL2 QUIET)
  endif()

  if(NOT TARGET SDL2::SDL2 AND NOT SDL2_FOUND)
    if(DEFINED SMK_SDL_ROOT)
      set(SDL2_INCLUDE_DIRS "${SMK_SDL_ROOT}/include" "${SMK_SDL_ROOT}/include/SDL2")
      find_library(SDL2_LIBRARIES NAMES SDL2 PATHS "${SMK_SDL_ROOT}/lib" NO_DEFAULT_PATH)
    endif()
  endif()
endif()

# Interactive Player Target (smk_play)
if(SMK_BUILD_PLAY)
  add_executable(smk_play src/desktop.c src/game_rtl.c ${SNESRECOMP_RUNNER_SOURCES})
  target_include_directories(smk_play PRIVATE
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    ${SNESRECOMP_ROOT}/runner/src/desktop
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

  if(TARGET SDL2::SDL2)
    target_link_libraries(smk_play PRIVATE SDL2::SDL2)
  else()
    target_include_directories(smk_play PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(smk_play PRIVATE ${SDL2_LIBRARIES})
  endif()

  target_compile_definitions(smk_play PRIVATE SDL_MAIN_HANDLED SNESRECOMP_TRACE=0 SNESRECOMP_ENABLE_MODS=0)
  if(NOT MSVC)
    target_compile_options(smk_play PRIVATE -w -Wno-implicit-function-declaration -ffunction-sections -fdata-sections)
    target_link_options(smk_play PRIVATE -Wl,--gc-sections)
  endif()
  target_link_libraries(smk_play PRIVATE snesrecomp_game ${SNESRECOMP_RUNNER_LIBRARIES} m)

  if(WIN32 AND DEFINED SMK_SDL_ROOT AND EXISTS "${SMK_SDL_ROOT}/bin/SDL2.dll")
    add_custom_command(TARGET smk_play POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${SMK_SDL_ROOT}/bin/SDL2.dll" "$<TARGET_FILE_DIR:smk_play>")
  endif()
endif()

# Headless Diagnostic Target (smk_headless)
if(SMK_BUILD_HEADLESS)
  add_executable(smk_headless src/headless.c src/game_rtl.c ${SNESRECOMP_RUNNER_SOURCES})
  target_include_directories(smk_headless PRIVATE
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    ${SNESRECOMP_ROOT}/runner/src/desktop
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

  if(TARGET SDL2::SDL2)
    target_link_libraries(smk_headless PRIVATE SDL2::SDL2)
  else()
    target_include_directories(smk_headless PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(smk_headless PRIVATE ${SDL2_LIBRARIES})
  endif()

  target_compile_definitions(smk_headless PRIVATE SDL_MAIN_HANDLED SNESRECOMP_TRACE=0 SNESRECOMP_ENABLE_MODS=0)
  if(NOT MSVC)
    target_compile_options(smk_headless PRIVATE -w -Wno-implicit-function-declaration -ffunction-sections -fdata-sections)
    target_link_options(smk_headless PRIVATE -Wl,--gc-sections)
  endif()
  target_link_libraries(smk_headless PRIVATE snesrecomp_game ${SNESRECOMP_RUNNER_LIBRARIES} m)

  if(WIN32 AND DEFINED SMK_SDL_ROOT AND EXISTS "${SMK_SDL_ROOT}/bin/SDL2.dll")
    add_custom_command(TARGET smk_headless POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${SMK_SDL_ROOT}/bin/SDL2.dll" "$<TARGET_FILE_DIR:smk_headless>")
  endif()
endif()
