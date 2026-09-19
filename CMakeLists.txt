cmake_minimum_required(VERSION 3.21)
project(Decomp2Gecko VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(DECOMP2GECKO_BUILD_TESTS "Build the unit tests" ON)
option(DECOMP2GECKO_BUILD_GUI "Build the Qt 6 Widgets GUI" ON)
option(DECOMP2GECKO_WERROR "Treat warnings as errors" OFF)

# shared warning set. -Wconversion left out on purpose bc the core mixes int64_t
# addresses w/ uint32_t instruction words everywhere & the noise would hide real
# problems
add_library(Decomp2Gecko_warnings INTERFACE)
target_compile_options(Decomp2Gecko_warnings INTERFACE
	$<$<CXX_COMPILER_ID:MSVC>:/W4 $<$<BOOL:${DECOMP2GECKO_WERROR}>:/WX>>
	$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Wshadow $<$<BOOL:${DECOMP2GECKO_WERROR}>:-Werror>>)
target_compile_definitions(Decomp2Gecko_warnings INTERFACE
	$<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)

execute_process(
	COMMAND git rev-parse --short HEAD
	WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
	OUTPUT_VARIABLE DECOMP2GECKO_GIT_HASH
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET)
if(NOT DECOMP2GECKO_GIT_HASH)
	set(DECOMP2GECKO_GIT_HASH "unknown")
endif()
execute_process(
	COMMAND git rev-parse --git-path HEAD
	WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
	OUTPUT_VARIABLE _GIT_HEAD_PATH
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET)
if(_GIT_HEAD_PATH AND NOT IS_ABSOLUTE "${_GIT_HEAD_PATH}")
	set(_GIT_HEAD_PATH "${CMAKE_SOURCE_DIR}/${_GIT_HEAD_PATH}")
endif()
if(_GIT_HEAD_PATH AND EXISTS "${_GIT_HEAD_PATH}")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_GIT_HEAD_PATH}")
	file(STRINGS "${_GIT_HEAD_PATH}" _GIT_HEAD_CONTENT LIMIT_COUNT 1)
	if(_GIT_HEAD_CONTENT MATCHES "^ref: (.*)")
		execute_process(
			COMMAND git rev-parse --git-path "${CMAKE_MATCH_1}"
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
			OUTPUT_VARIABLE _GIT_REF_PATH
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET)
		if(_GIT_REF_PATH AND NOT IS_ABSOLUTE "${_GIT_REF_PATH}")
			set(_GIT_REF_PATH "${CMAKE_SOURCE_DIR}/${_GIT_REF_PATH}")
		endif()
		if(_GIT_REF_PATH AND EXISTS "${_GIT_REF_PATH}")
			set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_GIT_REF_PATH}")
		endif()
	endif()
endif()
string(TIMESTAMP DECOMP2GECKO_BUILD_DATE "%Y%m%d")

configure_file(
	src/Decomp2Gecko/version.h.in
	"${CMAKE_CURRENT_BINARY_DIR}/generated/Decomp2Gecko/version.h"
	@ONLY)

add_library(Decomp2Gecko_core STATIC
	src/Decomp2Gecko/util.cpp
	src/Decomp2Gecko/json_mini.cpp
	src/Decomp2Gecko/elf.cpp
	src/Decomp2Gecko/vanilla.cpp
	src/Decomp2Gecko/reloc.cpp
	src/Decomp2Gecko/gameconfig.cpp
	src/Decomp2Gecko/decomp.cpp
	src/Decomp2Gecko/chunks.cpp
	src/Decomp2Gecko/layout.cpp
	src/Decomp2Gecko/layout_alias.cpp
	src/Decomp2Gecko/layout_patch.cpp
	src/Decomp2Gecko/patch.cpp
	src/Decomp2Gecko/emit.cpp
	src/Decomp2Gecko/slippi.cpp
	src/Decomp2Gecko/verify.cpp
	src/Decomp2Gecko/relink.cpp
	src/Decomp2Gecko/userini.cpp
	src/Decomp2Gecko/dol_extract.cpp
)
target_include_directories(Decomp2Gecko_core PUBLIC
	"${CMAKE_CURRENT_BINARY_DIR}/generated"
	"${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(Decomp2Gecko_core PUBLIC
	"DECOMP2GECKO_GIT_HASH=\"${DECOMP2GECKO_GIT_HASH}\""
	"DECOMP2GECKO_BUILD_DATE=\"${DECOMP2GECKO_BUILD_DATE}\"")
target_link_libraries(Decomp2Gecko_core PRIVATE Decomp2Gecko_warnings)

if(DECOMP2GECKO_BUILD_TESTS)
	enable_testing()
	add_executable(Decomp2Gecko_tests
		tests/main.cpp
		tests/test_util.cpp
		tests/test_ordered_map.cpp
		tests/test_json.cpp
		tests/test_gameconfig.cpp
		tests/test_elf.cpp
		tests/test_vanilla.cpp
		tests/test_reloc.cpp
		tests/test_chunks.cpp
		tests/test_emit.cpp
		tests/test_patch.cpp
		tests/test_slippi.cpp
		tests/test_userini.cpp
		tests/test_dol_extract.cpp
		tests/test_layout.cpp
	)
	target_link_libraries(Decomp2Gecko_tests PRIVATE Decomp2Gecko_core Decomp2Gecko_warnings)
	add_test(NAME Decomp2Gecko_unit COMMAND Decomp2Gecko_tests)
endif()

if(DECOMP2GECKO_BUILD_GUI)
	add_subdirectory(src/gui)
endif()

# packaging
set(CPACK_PACKAGE_NAME "Decomp2Gecko")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Turn decomp source mods into Gecko codes for the vanilla DOL")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Decomp2Gecko")
if(WIN32)
	set(CPACK_SYSTEM_NAME "win64")
	set(CPACK_NSIS_DISPLAY_NAME "Decomp2Gecko")
	set(CPACK_NSIS_PACKAGE_NAME "Decomp2Gecko")
	set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
	set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/packaging/Decomp2Gecko.ico")
	set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/packaging/Decomp2Gecko.ico")
	set(CPACK_NSIS_INSTALLED_ICON_NAME "Decomp2Gecko-gui.exe")
	set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
	set(CPACK_PACKAGE_EXECUTABLES "Decomp2Gecko-gui" "Decomp2Gecko")
elseif(APPLE)
	set(CPACK_SYSTEM_NAME "macos")
	set(CPACK_DMG_VOLUME_NAME "Decomp2Gecko")
endif()
include(CPack)
