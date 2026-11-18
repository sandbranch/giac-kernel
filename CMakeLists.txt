cmake_minimum_required(VERSION 3.16)
project(giac-kernel VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED IMPORTED_TARGET libzmq)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(nlohmann_json 3.9 REQUIRED)
find_package(Threads REQUIRED)

# libgiac ships no pkg-config or CMake files
find_path(GIAC_INCLUDE_DIR giac/giac.h REQUIRED)
find_library(GIAC_LIBRARY giac REQUIRED)
find_library(GMP_LIBRARY gmp REQUIRED)

# The protocol layer, shared by the kernel and the test client
add_library(wire STATIC src/wire.cc)
target_link_libraries(wire PUBLIC PkgConfig::ZMQ OpenSSL::Crypto nlohmann_json::nlohmann_json)

add_executable(giac-kernel src/main.cc src/engine.cc)
target_compile_definitions(giac-kernel PRIVATE GIAC_KERNEL_VERSION="${PROJECT_VERSION}")
target_include_directories(giac-kernel PRIVATE ${GIAC_INCLUDE_DIR})
target_link_libraries(giac-kernel PRIVATE wire ${GIAC_LIBRARY} ${GMP_LIBRARY} Threads::Threads)

install(TARGETS giac-kernel RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Kernelspec: clients find it in <prefix>/share/jupyter/kernels/giac
set(GIAC_KERNEL_BIN ${CMAKE_INSTALL_FULL_BINDIR}/giac-kernel)
configure_file(kernelspec/kernel.json.in kernelspec/kernel.json @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kernelspec/kernel.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/jupyter/kernels/giac)

include(CTest)
if(BUILD_TESTING)
  add_executable(protocol-test tests/protocol_test.cc)
  target_link_libraries(protocol-test PRIVATE wire)
  add_test(NAME protocol COMMAND protocol-test $<TARGET_FILE:giac-kernel>)
  set_tests_properties(protocol PROPERTIES TIMEOUT 120)
endif()

install(FILES doc/giac-kernel.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
