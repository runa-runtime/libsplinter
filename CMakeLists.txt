cmake_minimum_required(VERSION 3.16)
project(Splinter VERSION 1.2.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SPLINTER_VERSION_STRING "1.2.1")
add_definitions(-DSPLINTER_VERSION="${SPLINTER_VERSION_STRING}")

# --- Build Options ---
option(WITH_EMBEDDINGS "Enable Nomic/LLM embedding support" OFF)
option(WITH_NUMA       "Enable NUMA memory affinity support" OFF)
option(WITH_LUA        "Enable Lua CLI integration" OFF)
option(WITH_LLAMA      "Enable llama.cpp inference sidecar" OFF)
option(WITH_RUST       "Enable Rust bindings via Cargo" OFF)
option(WITH_WASM       "Enable WasmEdge CLI integration" OFF)


# --- Dependency Detection ---
find_package(PkgConfig REQUIRED)
find_program(VALGRIND_EXEC NAMES valgrind)

find_library(MATH_LIBRARY m)

if(WITH_LUA)
    pkg_check_modules(LUA lua5.4)
    if(LUA_FOUND)
        add_definitions(-DHAVE_LUA)
    else()
        message(FATAL_ERROR "Lua 5.4 not found. Disable WITH_LUA to skip.")
    endif()
endif()

if(WITH_NUMA)
    find_library(NUMA_LIB NAMES numa HINTS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib)
    find_path(NUMA_INCLUDE_DIR NAMES numa.h HINTS /usr/include)
    if(NUMA_LIB AND NUMA_INCLUDE_DIR)
        # SPLINTER_NUMA_AFFINITY will be deprecated in favor of HAVE_NUMA
        add_definitions(-DSPLINTER_NUMA_AFFINITY)
        add_definitions(-DHAVE_NUMA)
    else()
        message(FATAL_ERROR "libnuma-dev not found. Disable WITH_NUMA to skip.")
    endif()
endif()

if(WITH_WASM)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WasmEdge QUIET wasmedge)
    if(NOT WasmEdge_FOUND)
        # 2. Fallback to manual search if PkgConfig fails (common for ~/.wasmedge installs)
        find_path(WasmEdge_INCLUDE_DIR NAMES wasmedge/wasmedge.h
            HINTS "$ENV{HOME}/.wasmedge/include" /usr/local/include /usr/include)
        find_library(WasmEdge_LIBRARIES NAMES wasmedge
            HINTS "$ENV{HOME}/.wasmedge/lib" /usr/local/lib /usr/lib) 
        if(WasmEdge_INCLUDE_DIR AND WasmEdge_LIBRARIES)
            set(WasmEdge_FOUND TRUE)
        endif()
    endif()
    if(WasmEdge_FOUND)
        message(STATUS "Found WasmEdge: ${WasmEdge_LIBRARIES}")
        add_library(wasmedge_lib INTERFACE)
        target_include_directories(wasmedge_lib INTERFACE 
            ${WasmEdge_INCLUDE_DIRS} ${WasmEdge_INCLUDE_DIR})
        target_link_libraries(wasmedge_lib INTERFACE 
            ${WasmEdge_LIBRARIES})    
        add_definitions(-DHAVE_WASM)
    else()
        message(FATAL_ERROR "WasmEdge not found. Install it via 'curl -sSf https://raw.githubusercontent.com/WasmEdge/WasmEdge/master/utils/install.sh | bash' or set -DWITH_WASM=OFF")
    endif()
endif()

find_path(VALGRIND_INCLUDE_DIR names valgrind/valgrind.h)
if(VALGRIND_INCLUDE_DIR)
    add_definitions(-DHAVE_VALGRIND_H)
    add_definitions(-DHAVE_VALGRIND)
endif()

add_compile_options(-O3 -Wall -Wextra -D_GNU_SOURCE -fPIC)

# Resolve install dirs early so CMAKE_INSTALL_FULL_LIBDIR is available before
# target creation. This lets us bake the install RPATH into binaries at build
# time via BUILD_WITH_INSTALL_RPATH, avoiding the patchelf dependency that
# cmake's RPATH_CHANGE file() command requires on ELF platforms.
include(GNUInstallDirs)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

if(WITH_EMBEDDINGS)
    add_definitions(-DSPLINTER_EMBEDDINGS)
    add_definitions(-DHAVE_EMBEDDINGS)
endif()

# --- Library Targets ---
add_library(splinter_shared SHARED splinter.c)
set_target_properties(splinter_shared PROPERTIES OUTPUT_NAME splinter)

if(WITH_NUMA)
    target_link_libraries(splinter_shared PRIVATE ${NUMA_LIB})
    target_include_directories(splinter_shared PRIVATE ${NUMA_INCLUDE_DIR})
endif()

if(WITH_WASM)
    # This single line handles both the .so linking and the header search paths
    target_link_libraries(splinter_shared PRIVATE wasmedge_lib)
    target_compile_definitions(splinter_shared PRIVATE HAVE_WASM)
endif()

# Persistent Core Objects and Library
add_library(splinter_p_obj OBJECT splinter.c)
target_compile_definitions(splinter_p_obj PRIVATE SPLINTER_PERSISTENT)
target_compile_options(splinter_p_obj PRIVATE -fPIC)

add_library(splinter_p_shared SHARED $<TARGET_OBJECTS:splinter_p_obj>)
set_target_properties(splinter_p_shared PROPERTIES OUTPUT_NAME splinter_p)

if(WITH_NUMA)
    target_link_libraries(splinter_p_shared PRIVATE ${NUMA_LIB})
endif()

# --- CLI Targets ---
file(GLOB CLI_SOURCES "splinter_cli_*.c")

add_executable(splinter_cli ${CLI_SOURCES} 3rdparty/linenoise.c 3rdparty/libgrawk.c 3rdparty/argparse.c 3rdparty/uuid4.c)
target_link_libraries(splinter_cli PRIVATE splinter_shared ${LUA_LDFLAGS} ${MATH_LIBRARY})
target_include_directories(splinter_cli PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty
    ${LUA_INCLUDE_DIRS}
)

add_executable(splinterp_cli ${CLI_SOURCES} 3rdparty/linenoise.c 3rdparty/libgrawk.c 3rdparty/argparse.c 3rdparty/uuid4.c)
target_compile_definitions(splinterp_cli PRIVATE SPLINTER_PERSISTENT)
target_include_directories(splinterp_cli PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty 
    ${LUA_INCLUDE_DIRS}
)
target_link_libraries(splinterp_cli PRIVATE splinter_p_shared ${LUA_LDFLAGS} ${MATH_LIBRARY})

# --- llama.cpp Inference Sidecar ---
if(WITH_LLAMA)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 14)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    find_library(LLAMA_LIB NAMES llama HINTS /usr/local/lib /usr/lib /opt/llama.cpp/build)
    find_path(LLAMA_INCLUDE_DIR NAMES llama.h HINTS /usr/local/include /usr/include /opt/llama.cpp)

    if(LLAMA_LIB AND LLAMA_INCLUDE_DIR)
        add_definitions(-DHAVE_LLAMA)
        add_executable(splinference splinference.cpp)
        target_include_directories(splinference PRIVATE ${LLAMA_INCLUDE_DIR})
        target_link_libraries(splinference PRIVATE splinter_shared ${LLAMA_LIB})
        
        add_executable(splinferencep splinference.cpp)
        target_compile_definitions(splinferencep PRIVATE SPLINTER_PERSISTENT)
        target_include_directories(splinferencep PRIVATE ${LLAMA_INCLUDE_DIR})
        target_link_libraries(splinferencep PRIVATE splinter_p_shared ${LLAMA_LIB})

        add_executable(splainference splainference.cpp)
        target_include_directories(splainference PRIVATE ${LLAMA_INCLUDE_DIR})
        target_link_libraries(splainference PRIVATE splinter_shared ${LLAMA_LIB})

        add_executable(splainferencep splainference.cpp)
        target_compile_definitions(splainferencep PRIVATE SPLINTER_PERSISTENT)
        target_include_directories(splainferencep PRIVATE ${LLAMA_INCLUDE_DIR})
        target_link_libraries(splainferencep PRIVATE splinter_p_shared ${LLAMA_LIB})
    else()
        message(FATAL_ERROR "llama.cpp not found. Disable WITH_LLAMA to skip.")
    endif()
endif()

# --- Installation Logic ---
install(TARGETS splinter_shared splinter_p_shared
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(TARGETS splinter_cli splinterp_cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WITH_LLAMA AND TARGET splinference AND TARGET splinferencep)
    install(TARGETS splinference splinferencep RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(TARGETS splainference splainferencep RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(WITH_WASM)
    target_link_libraries(splinter_cli PRIVATE wasmedge_lib)
    target_link_libraries(splinterp_cli PRIVATE wasmedge_lib)
endif()

install(FILES splinter.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(CODE "
    execute_process(COMMAND ln -sf ${CMAKE_INSTALL_FULL_BINDIR}/splinter_cli ${CMAKE_INSTALL_FULL_BINDIR}/splinterctl)
    message(STATUS \"Created symlink: splinterctl -> splinter_cli\")

    execute_process(COMMAND ln -sf ${CMAKE_INSTALL_FULL_BINDIR}/splinterp_cli ${CMAKE_INSTALL_FULL_BINDIR}/splinterpctl)
    message(STATUS \"Created symlink: splinterpctl -> splinterp_cli\")
")

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    install(CODE "execute_process(COMMAND ldconfig)")
endif()

# --- Local Development Symlinks ---
add_custom_command(TARGET splinter_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink $<TARGET_FILE_NAME:splinter_cli> ${CMAKE_CURRENT_BINARY_DIR}/splinterctl)

add_custom_command(TARGET splinterp_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink $<TARGET_FILE_NAME:splinterp_cli> ${CMAKE_CURRENT_BINARY_DIR}/splinterpctl)

# --- Rust Bindings ---
if(WITH_RUST)
    find_program(CARGO_EXEC NAMES cargo)
    if(CARGO_EXEC)
        set(RUST_ENV "CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/rust_target")
        set(RUST_LINK_SEARCH "SPLINTER_LIB_DIR=${CMAKE_CURRENT_BINARY_DIR}")
        add_custom_target(rust_bindings ALL
            COMMAND ${CMAKE_COMMAND} -E env ${RUST_ENV} ${RUST_LINK_SEARCH} ${CARGO_EXEC} build --release
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bindings/rust
            DEPENDS splinter_shared splinter_p_shared
        )
    endif()
endif()

# --- Local Symlinks for Development/Testing ---

# Symlink for splinter_cli -> splinterctl
add_custom_command(
    TARGET splinter_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink 
            $<TARGET_FILE_NAME:splinter_cli> 
            ${CMAKE_CURRENT_BINARY_DIR}/splinterctl
    COMMENT "Creating local symlink: splinterctl"
)

# Symlink for splinterp_cli -> splinterpctl
add_custom_command(
    TARGET splinterp_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink 
            $<TARGET_FILE_NAME:splinterp_cli> 
            ${CMAKE_CURRENT_BINARY_DIR}/splinterpctl
    COMMENT "Creating local symlink: splinterpctl"
)

# The lua test suite
set(LUA_TEST_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/test.lua")
set(LUA_TEST_BINARY "${CMAKE_CURRENT_BINARY_DIR}/test.lua")
add_custom_command(
    OUTPUT "${LUA_TEST_BINARY}"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${LUA_TEST_SOURCE}" "${LUA_TEST_BINARY}"
    DEPENDS "${LUA_TEST_SOURCE}"
    COMMENT "Symlinking test.lua to build directory"
)
add_custom_target(copy_test_lua ALL DEPENDS "${LUA_TEST_BINARY}")

set(SPLINTERCTL_TEST_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/splinterctl_tests.sh")
set(SPLINTERCTL_TEST_BINARY "${CMAKE_CURRENT_BINARY_DIR}/splinterctl_tests.sh")
add_custom_command(
    OUTPUT "${SPLINTERCTL_TEST_BINARY}"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${SPLINTERCTL_TEST_SOURCE}" "${SPLINTERCTL_TEST_BINARY}"
    DEPENDS "${SPLINTERCTL_TEST_SOURCE}"
    COMMENT "Symlinking splinterctl_tests.sh to build directory"
)
add_custom_target(copy_test_splinterctl ALL DEPENDS "${SPLINTERCTL_TEST_BINARY}")

enable_testing()

add_executable(splinter_test splinter_test.c)
target_link_libraries(splinter_test PRIVATE splinter_shared)

add_executable(splinter_stress splinter_stress.c)
target_link_libraries(splinter_stress PRIVATE splinter_shared)

add_executable(splinterp_test splinter_test.c)
target_compile_definitions(splinterp_test PRIVATE SPLINTER_PERSISTENT)
target_link_libraries(splinterp_test PRIVATE splinter_p_shared)

add_executable(splinterp_stress splinter_stress.c)
target_compile_definitions(splinterp_stress PRIVATE SPLINTER_PERSISTENT)
target_link_libraries(splinterp_stress PRIVATE splinter_p_shared)

add_executable(splinter_chi_sao splinter_chi_sao.c)
target_link_libraries(splinter_chi_sao PRIVATE splinter_shared)

add_executable(splinterp_chi_sao splinter_chi_sao.c)
target_compile_definitions(splinterp_chi_sao PRIVATE SPLINTER_PERSISTENT)
target_link_libraries(splinterp_chi_sao PRIVATE splinter_p_shared)

add_executable(sidecar sidecar.c)
target_link_libraries(sidecar PRIVATE splinter_shared)

add_executable(sidecarp sidecar.c)
target_compile_definitions(sidecarp PRIVATE SPLINTER_PERSISTENT)
target_link_libraries(sidecarp PRIVATE splinter_p_shared)

set_target_properties(
    splinter_test
    splinter_stress
    splinter_chi_sao
    splinterp_test
    splinterp_stress
    splinterp_chi_sao
    PROPERTIES
    SKIP_BUILD_RPATH FALSE
    BUILD_WITH_INSTALL_RPATH FALSE
)

# 3. Register Tests (The fix for "No tests were found!!!")
add_test(NAME standard_unit_test COMMAND splinter_test)
add_test(NAME persistent_unit_test COMMAND splinterp_test)
add_test(NAME standard_stress_test COMMAND splinter_stress --duration-ms 7500)
add_test(NAME standard_chi_sao_test COMMAND splinter_chi_sao --duration-ms 7500 --writers 4)

# Valgrind Tests (only added if valgrind is found)
if(VALGRIND_EXEC)
    # Replicate your Makefile's flags: -s (silent) and --leak-check=full
    set(VALGRIND_ARGS "-s" "--leak-check=full" "--error-exitcode=1")

    add_test(NAME "MemCheck_Core" 
             COMMAND ${VALGRIND_EXEC} ${VALGRIND_ARGS} ./splinter_test)
             
    add_test(NAME "MemCheck_Persistent" 
             COMMAND ${VALGRIND_EXEC} ${VALGRIND_ARGS} ./splinterp_test)

    add_test(NAME "MemCheck_CLI_Regression"
            COMMAND ${VALGRIND_EXEC} /bin/bash ./splinterctl_tests.sh)

endif()

# 4. Install Targets
install(TARGETS splinter_test splinter_stress splinter_chi_sao splinterp_test splinterp_stress splinterp_chi_sao sidecar sidecarp
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES splinterrc_example DESTINATION ${CMAKE_INSTALL_DATADIR}/splinter RENAME splinterrc.example)
install(FILES bindings/ts/splinter.ts DESTINATION ${CMAKE_INSTALL_DATADIR}/splinter)

install(CODE "
    message(STATUS \"\")
    message(STATUS \"------------------------------------------------------------------\")
    message(STATUS \"  CONFIGURATION NOTE:\")
    message(STATUS \"  A sample bloom configuration file for Splinter has been installed to:\")
    message(STATUS \"  ${CMAKE_INSTALL_FULL_DATADIR}/splinter/splinterrc.example\")
    message(STATUS \"\")
    message(STATUS \"  To customize Splinter's bloom label functionality, copy this to:\")
    message(STATUS \"  ~/.splinterrc\")
    message(STATUS \"------------------------------------------------------------------\")
    message(STATUS \"\")
")