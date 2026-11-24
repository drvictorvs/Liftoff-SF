cmake_minimum_required(VERSION 3.24)

project(Liftoff VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the C++ runtime statically, so players don't need any extra redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# MinHook: small library that "hooks" Windows functions. Downloaded automatically
# the first time you configure (needs internet once). No Git required.
# ---------------------------------------------------------------------------
include(FetchContent)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)  # MinHook's CMake file is old; this keeps new CMake happy
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)  # ...and lets it use the same static runtime as us

# ---------------------------------------------------------------------------
# CommonLibSF: used to detect when the player is present in the loaded world.
# Its source is GPL-3.0-or-later with a modding/linking exception; package_source
# below includes the corresponding sources and licenses for releases.
# ---------------------------------------------------------------------------
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
	URL https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.zip
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(spdlog)
set(spdlog_DIR "${spdlog_BINARY_DIR}" CACHE PATH "spdlog package directory" FORCE)

FetchContent_Declare(commonlib_shared
	URL https://github.com/libxse/commonlib-shared/archive/def5f6c93db28d20307d8e55a62118fc1c4f0b6c.zip
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(commonlib_shared)

FetchContent_Declare(commonlibsf
	URL https://github.com/libxse/commonlibsf/archive/d7a15706e86ec4c1f9423a5a33bc352d9f58c08e.zip
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(commonlibsf)
if(NOT commonlibsf_POPULATED)
	FetchContent_Populate(commonlibsf)
endif()

file(GLOB_RECURSE COMMONLIBSF_SOURCES CONFIGURE_DEPENDS
	"${commonlibsf_SOURCE_DIR}/src/*.cpp"
)
add_library(commonlibsf STATIC ${COMMONLIBSF_SOURCES})
target_compile_features(commonlibsf PUBLIC cxx_std_23)
target_include_directories(commonlibsf PUBLIC "${commonlibsf_SOURCE_DIR}/include")
target_compile_definitions(commonlibsf PUBLIC UNICODE _UNICODE NOMINMAX)
target_link_libraries(commonlibsf PUBLIC commonlib-shared)
target_precompile_headers(commonlibsf PRIVATE "${commonlibsf_SOURCE_DIR}/include/SFSE/Impl/PCH.h")

FetchContent_Declare(minhook
	URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(minhook)

# ---------------------------------------------------------------------------
# The plugin itself: a DLL called Liftoff.dll
# ---------------------------------------------------------------------------
add_library(Liftoff SHARED
	src/main.cpp
	src/Config.cpp
	src/OsfSettings.cpp
	src/FileWatch.cpp
	src/FileTrace.cpp
	src/Log.cpp
	src/Progress.cpp
	src/Stacks.cpp
	src/Splash.cpp
	src/Watchdog.cpp
	src/GameReady.cpp
)

target_compile_definitions(Liftoff PRIVATE
	UNICODE _UNICODE      # use the wide-character (W) versions of Windows functions
	NOMINMAX              # stop windows.h from defining min/max macros
	_WIN32_WINNT=0x0A00   # target Windows 10+
	# The version in project() above is the only place to change it; the DLL reports it to SFSE.
	LIFTOFF_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
	LIFTOFF_VERSION_MINOR=${PROJECT_VERSION_MINOR}
	LIFTOFF_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_link_libraries(Liftoff PRIVATE minhook commonlibsf gdiplus dxgi psapi user32 gdi32 shell32 ole32 uuid)

if(MSVC)
	target_compile_options(Liftoff PRIVATE /W4 /utf-8 /permissive-)
endif()

# ---------------------------------------------------------------------------
# Optional: copy Liftoff.dll into the game after every build.
# Set STARFIELD_DIR in .vscode/settings.json (cmake.configureSettings).
# ---------------------------------------------------------------------------
set(STARFIELD_DIR "" CACHE PATH "Starfield install folder; if set, the DLL is copied there after each build")
if(STARFIELD_DIR)
	add_custom_command(TARGET Liftoff POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${STARFIELD_DIR}/Data/SFSE/Plugins"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Liftoff>" "${STARFIELD_DIR}/Data/SFSE/Plugins/"
		COMMENT "Deploying Liftoff.dll to ${STARFIELD_DIR}/Data/SFSE/Plugins"
	)
endif()

# ---------------------------------------------------------------------------
# "package_mod" target: builds the zip you upload to Nexus.
#   CMake Tools: "CMake: Build Target" -> package_mod
#   Terminal:    cmake --build build --config Release --target package_mod
# ---------------------------------------------------------------------------
set(PACKAGE_DIR "${CMAKE_BINARY_DIR}/package")
set(PACKAGE_ZIP "${CMAKE_BINARY_DIR}/Liftoff-${PROJECT_VERSION}.zip")
add_custom_target(package_mod
	COMMAND ${CMAKE_COMMAND} -E rm -rf "${PACKAGE_DIR}"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/dist" "${PACKAGE_DIR}"
	COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:Liftoff>" "${PACKAGE_DIR}/Data/SFSE/Plugins/"
	COMMAND ${CMAKE_COMMAND} -E rm -f "${PACKAGE_ZIP}"
	COMMAND ${CMAKE_COMMAND} -E chdir "${PACKAGE_DIR}" ${CMAKE_COMMAND} -E tar cf "${PACKAGE_ZIP}" --format=zip Data
	COMMENT "Creating ${PACKAGE_ZIP}"
	DEPENDS Liftoff
)

# CommonLibSF's linking exception requires distributing corresponding source
# with plugin binaries. Keep it separate from the installable game-data zip.
set(SOURCE_PACKAGE_DIR "${CMAKE_BINARY_DIR}/source-package")
set(SOURCE_PACKAGE_ZIP "${CMAKE_BINARY_DIR}/Liftoff-${PROJECT_VERSION}-source.zip")
add_custom_target(package_source
	COMMAND ${CMAKE_COMMAND} -E rm -rf "${SOURCE_PACKAGE_DIR}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${SOURCE_PACKAGE_DIR}/src" "${SOURCE_PACKAGE_DIR}/scripts" "${SOURCE_PACKAGE_DIR}/dist" "${SOURCE_PACKAGE_DIR}/third_party"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/src" "${SOURCE_PACKAGE_DIR}/src"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/scripts" "${SOURCE_PACKAGE_DIR}/scripts"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/dist" "${SOURCE_PACKAGE_DIR}/dist"
	COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/CMakeLists.txt" "${CMAKE_SOURCE_DIR}/README.md" "${CMAKE_SOURCE_DIR}/compile_papyrus.bat" "${SOURCE_PACKAGE_DIR}/"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${commonlibsf_SOURCE_DIR}" "${SOURCE_PACKAGE_DIR}/third_party/commonlibsf"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${commonlib_shared_SOURCE_DIR}" "${SOURCE_PACKAGE_DIR}/third_party/commonlib-shared"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${spdlog_SOURCE_DIR}" "${SOURCE_PACKAGE_DIR}/third_party/spdlog"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${minhook_SOURCE_DIR}" "${SOURCE_PACKAGE_DIR}/third_party/minhook"
	COMMAND ${CMAKE_COMMAND} -E rm -f "${SOURCE_PACKAGE_ZIP}"
	COMMAND ${CMAKE_COMMAND} -E chdir "${SOURCE_PACKAGE_DIR}" ${CMAKE_COMMAND} -E tar cf "${SOURCE_PACKAGE_ZIP}" --format=zip .
	COMMENT "Creating corresponding source archive ${SOURCE_PACKAGE_ZIP}"
)
