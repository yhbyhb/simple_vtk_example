# UseVcpkg.cmake - Use vcpkg as a dependency
include(FetchContent)

# Path to the vcpkg-configuration.json file
set(VCPKG_CONFIG_FILE "${CMAKE_SOURCE_DIR}/vcpkg-configuration.json")

# Function to extract the baseline for the "default-registry"
function(get_default_registry_baseline json_file git_commit_hash)
  file(READ ${json_file} json_content) # Read the entire JSON file as a string

  # Match the default-registry baseline field
  string(REGEX MATCH
               "\"default-registry\"[^{]*{[^}]*\"baseline\": \"([^\"]+)\"" _
               ${json_content})

  # Check if the regex matched and return the commit hash
  if(CMAKE_MATCH_1)
    set(${git_commit_hash}
        ${CMAKE_MATCH_1}
        PARENT_SCOPE)
  else()
    message(
      FATAL_ERROR
        "Could not find the baseline for the default-registry in ${json_file}")
  endif()
endfunction()

# Call the function to extract the baseline from the default-registry
get_default_registry_baseline(${VCPKG_CONFIG_FILE} VCPKG_GIT_COMMIT_HASH)

message(
  STATUS
    "Using VCPKG commit hash in default-registry: ${VCPKG_GIT_COMMIT_HASH}")

# Use the extracted Git commit hash in FetchContent
FetchContent_Declare(
  vcpkg
  GIT_REPOSITORY https://github.com/microsoft/vcpkg.git
  GIT_TAG ${VCPKG_GIT_COMMIT_HASH}
)

FetchContent_MakeAvailable(vcpkg)

# Set vcpkg toolchain file
set(CMAKE_TOOLCHAIN_FILE ${vcpkg_SOURCE_DIR}/scripts/buildsystems/vcpkg.cmake)
