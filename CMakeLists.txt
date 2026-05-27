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
else()
    message(WARNING "lite3-cpp not found at ${LITE3CPP_DIR}. Control plane features will be disabled.")
endif()

# 4. liberasurecode (Assume system or fetch if needed)
# For now, let's try to find it.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBERASURECODE liberasurecode-1)
if(NOT LIBERASURECODE_FOUND)
    message(WARNING "liberasurecode not found. Phase 1.1.2 will require manual installation or FetchContent.")
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
    ${LIBERASURECODE_INCLUDE_DIRS}
)

target_link_libraries(erasurecoding PRIVATE
    irods_common
    conveyor
    ${LIBERASURECODE_LIBRARIES}
)

if(TARGET lite3cpp)
    target_link_libraries(erasurecoding PRIVATE lite3cpp)
    target_compile_definitions(erasurecoding PRIVATE HAS_LITE3CPP)
endif()

# --- Installation ---
install(TARGETS erasurecoding
    LIBRARY DESTINATION /usr/lib/irods/plugins/resources
)
