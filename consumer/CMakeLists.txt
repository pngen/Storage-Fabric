cmake_minimum_required(VERSION 3.21)

project(StorageFabricConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Consume Storage Fabric purely as an installed CMake package.
find_package(StorageFabric CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE StorageFabric::storagefabric)
