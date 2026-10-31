cmake_minimum_required(VERSION 3.20)
project(shrink LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SHRINK_BUILD_GUI "Build Qt6" ON)

include(FetchContent)
FetchContent_Declare(
	re2
	GIT_REPOSITORY https://github.com/google/re2.git
	GIT_TAG 2024-07-02
	GIT_SHALLOW TRUE
)
set(RE2_BUILD_TESTING OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(re2)

if(TARGET re2)
	target_compile_options(re2 PRIVATE -w)
endif()

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_library(shrink_core STATIC
	shrink_engine.cpp
	shrink_engine.hpp
)

target_compile_features(shrink_core PUBLIC cxx_std_20)
target_compile_options(shrink_core PRIVATE
	$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -pedantic -O3>
	$<$<CXX_COMPILER_ID:MSVC>:/W4 /O2>
)
target_include_directories(shrink_core PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(shrink_core PUBLIC
	re2::re2
	nlohmann_json
)

add_executable(shrink_cli
	shrink.cpp
)
target_compile_features(shrink_cli PRIVATE cxx_std_20)
target_compile_options(shrink_cli PRIVATE
	$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -pedantic -O3>
	$<$<CXX_COMPILER_ID:MSVC>:/W4 /O2>
)
target_link_libraries(shrink_cli PRIVATE
	shrink_core
)
set_target_properties(shrink_cli PROPERTIES OUTPUT_NAME "shrink")

if(SHRINK_BUILD_GUI)
	set(CMAKE_AUTOMOC ON)
	set(CMAKE_AUTORCC ON)

	find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)

	set(GUI_SOURCES
		main.cpp
		mainwindow.cpp
		mainwindow.h
		shrink_controller.cpp
		shrink_controller.h
		code_editor_view.cpp
		code_editor_view.h
		options_view.cpp
		options_view.h
		batch_queue_view.cpp
		batch_queue_view.h
		custom_rules_view.cpp
		custom_rules_view.h
		metrics_bundle_view.cpp
		metrics_bundle_view.h
	)

	add_executable(shrink_gui ${GUI_SOURCES})

	target_compile_features(shrink_gui PRIVATE cxx_std_20)
	target_compile_options(shrink_gui PRIVATE
		$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -pedantic -O3>
		$<$<CXX_COMPILER_ID:MSVC>:/W4 /O2>
	)

	target_link_libraries(shrink_gui PRIVATE
		shrink_core
		Qt6::Core
		Qt6::Gui
		Qt6::Widgets
		Qt6::Concurrent
	)
	set_target_properties(shrink_gui PROPERTIES OUTPUT_NAME "shrink-gui")
endif()
