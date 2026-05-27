cmake_minimum_required(VERSION 3.20)
project(irods_resource_plugin_erasurecoding VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- Dependencies ---

# 1. iRODS
find_path(IRODS_INCLUDE_DIR NAMES irods/rods.h PATHS /usr/include/irods)
if(NOT IRODS_INCLUDE_DIR)
    message(FATAL_ERROR "iRODS headers not found. Please install irods-dev.")
endif()
include_directories(/usr/include/irods)

# 2. libconveyor (Remote Fetch)
include(FetchContent)
FetchContent_Declare(
  libconveyor
  GIT_REPOSITORY https://github.com/jasoncoposky/libconveyor.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(libconveyor)

# 3. lite3-cpp (Local)
set(LITE3CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../lib/lite3-cpp)
if(EXISTS ${LITE3CPP_DIR})
    add_subdirectory(${LITE3CPP_DIR} lite3-cpp)
    include_directories(${LITE3CPP_DIR}/include)
endif()

# 5. liblite3client (Local)
set(LITE3CLIENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../lib/liblite3client)
if(EXISTS ${LITE3CLIENT_DIR})
    add_subdirectory(${LITE3CLIENT_DIR} liblite3client)
    include_directories(${LITE3CLIENT_DIR}/include)
endif()

# --- Target: Erasure Coding Resource Plugin ---

add_library(erasurecoding SHARED
    src/liberasurecoding_resource.cpp
)

set_target_properties(erasurecoding PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "erasurecoding"
)

target_include_directories(erasurecoding PRIVATE 
    include
    ${LIBERASURECODE_INCLUDE_DIR}
)

target_link_libraries(erasurecoding PRIVATE
    irods_common
    conveyor
    ${LIBERASURECODE_LIBRARY}
)

if(TARGET lite3cpp)
    target_link_libraries(erasurecoding PRIVATE lite3cpp)
    target_compile_definitions(erasurecoding PRIVATE HAS_LITE3CPP)
endif()

if(TARGET lite3client)
    target_link_libraries(erasurecoding PRIVATE lite3client)
    target_compile_definitions(erasurecoding PRIVATE HAS_LITE3CLIENT)
endif()

# --- Unit Tests ---
enable_testing()
add_executable(test_erasurecoder test/test_erasurecoder.cpp)
target_include_directories(test_erasurecoder PRIVATE include ${LIBERASURECODE_INCLUDE_DIR})
target_link_libraries(test_erasurecoder PRIVATE 
    gtest_main 
    ${LIBERASURECODE_LIBRARY}
)
add_test(NAME test_erasurecoder COMMAND test_erasurecoder)

add_executable(test_plugin_simulation test/test_plugin_simulation.cpp src/liberasurecoding_resource.cpp)
target_include_directories(test_plugin_simulation PRIVATE include ${LIBERASURECODE_INCLUDE_DIR})
target_link_libraries(test_plugin_simulation PRIVATE 
    gtest_main 
    irods_server
    irods_common 
    conveyor
    fmt
    ${LIBERASURECODE_LIBRARY}
)
add_test(NAME test_plugin_simulation COMMAND test_plugin_simulation)

# --- Installation ---
install(TARGETS erasurecoding
    LIBRARY DESTINATION /usr/lib/irods/plugins/resources
)
