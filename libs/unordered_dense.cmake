# ---------------------------------------------------------------------------
# leanstore
# ---------------------------------------------------------------------------
# Implemented for RecordCache's Hash Table, using third party library

include(ExternalProject)
find_package(Git REQUIRED)

# Get unordered_dense
ExternalProject_Add(
        unordered_dense_src
        PREFIX "vendor/unordered_dense"
        GIT_REPOSITORY "https://github.com/martinus/unordered_dense.git"
        GIT_TAG v4.8.1
        TIMEOUT 10
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        UPDATE_COMMAND ""
)

# Prepare json
ExternalProject_Get_Property(unordered_dense_src source_dir)
set(UNORDERED_DENSE_INCLUDE_DIR ${source_dir}/include)
file(MAKE_DIRECTORY ${UNORDERED_DENSE_INCLUDE_DIR})
add_library(unordered_dense INTERFACE)
set_property(TARGET unordered_dense APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${UNORDERED_DENSE_INCLUDE_DIR})

# Dependencies
add_dependencies(unordered_dense unordered_dense_src)
