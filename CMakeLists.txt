cmake_minimum_required(VERSION 3.21)

project(StorageFabric
    VERSION 1.0.0
    DESCRIPTION "Governed AI storage placement runtime"
    LANGUAGES CXX)

# ---- Options ----------------------------------------------------------------
option(STORAGEFABRIC_BUILD_TESTS "Build Storage Fabric tests" ON)
option(STORAGEFABRIC_BUILD_EXAMPLES "Build Storage Fabric examples" ON)
option(STORAGEFABRIC_BUILD_BENCHMARKS "Build Storage Fabric benchmarks" ON)
option(STORAGEFABRIC_ENABLE_SYNTHETIC_BACKENDS "Enable deterministic synthetic backends" ON)
option(STORAGEFABRIC_ENABLE_CUDA_PROOF "Enable the CUDA staging proof" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4 /WX /permissive- /Zc:__cplusplus)
    add_compile_options(/EHsc)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ---- Gather sources ---------------------------------------------------------
file(GLOB STORAGEFABRIC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(storagefabric_lib STATIC ${STORAGEFABRIC_SOURCES})
add_library(StorageFabric::storagefabric ALIAS storagefabric_lib)
set_target_properties(storagefabric_lib PROPERTIES
    EXPORT_NAME storagefabric
    POSITION_INDEPENDENT_CODE ON
    FOLDER "storagefabric")

target_include_directories(storagefabric_lib
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(storagefabric_lib PUBLIC cxx_std_20)
target_compile_definitions(storagefabric_lib PUBLIC STORAGEFABRIC_VERSION="${PROJECT_VERSION}")

if(STORAGEFABRIC_ENABLE_SYNTHETIC_BACKENDS)
    target_compile_definitions(storagefabric_lib PUBLIC STORAGEFABRIC_HAS_SYNTHETIC_BACKENDS=1)
endif()

# ---- Tests ------------------------------------------------------------------
if(STORAGEFABRIC_BUILD_TESTS)
    enable_testing()
    file(GLOB STORAGEFABRIC_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test/*.cpp")
    foreach(test_src ${STORAGEFABRIC_TEST_SOURCES})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        target_link_libraries(${test_name} PRIVATE StorageFabric::storagefabric)
        target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test")
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

# ---- Examples ---------------------------------------------------------------
if(STORAGEFABRIC_BUILD_EXAMPLES)
    file(GLOB STORAGEFABRIC_EXAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp")
    foreach(example_src ${STORAGEFABRIC_EXAMPLE_SOURCES})
        get_filename_component(example_name ${example_src} NAME_WE)
        add_executable(${example_name} ${example_src})
        target_link_libraries(${example_name} PRIVATE StorageFabric::storagefabric)
    endforeach()
endif()

# ---- Benchmarks -------------------------------------------------------------
if(STORAGEFABRIC_BUILD_BENCHMARKS)
    file(GLOB STORAGEFABRIC_BENCH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cpp")
    foreach(bench_src ${STORAGEFABRIC_BENCH_SOURCES})
        get_filename_component(bench_name ${bench_src} NAME_WE)
        add_executable(${bench_name} ${bench_src})
        target_link_libraries(${bench_name} PRIVATE StorageFabric::storagefabric)
    endforeach()
endif()

# ---- Tool executables (CLI, coordinator, worker) -----------------------------
file(GLOB STORAGEFABRIC_TOOL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/*.cpp")
foreach(tool_src ${STORAGEFABRIC_TOOL_SOURCES})
    get_filename_component(tool_name ${tool_src} NAME_WE)
    add_executable(${tool_name} ${tool_src})
    target_link_libraries(${tool_name} PRIVATE StorageFabric::storagefabric)
    set_target_properties(${tool_name} PROPERTIES FOLDER "tools")
endforeach()

# ---- Install ----------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS storagefabric_lib
    EXPORT StorageFabricTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT StorageFabricTargets
    FILE StorageFabricTargets.cmake
    NAMESPACE StorageFabric::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/StorageFabric)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StorageFabricConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/StorageFabricConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/StorageFabric)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/StorageFabricConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/StorageFabricConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/StorageFabricConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/StorageFabric)

set(CPACK_PACKAGE_NAME "StorageFabric")
set(CPACK_PACKAGE_VENDOR "Summon Software Labs")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Governed AI storage placement runtime")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "StorageFabric-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
if(NOT WIN32)
    set(CPACK_GENERATOR "TGZ")
else()
    set(CPACK_GENERATOR "ZIP")
endif()
include(CPack)
