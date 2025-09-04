cmake_minimum_required(VERSION 3.10)
# Use vcpkg
include(UseVcpkg.cmake)

project(simple_vtk_example LANGUAGES CXX)

# Specify C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(spdlog CONFIG REQUIRED)
find_package(VTK REQUIRED)

add_executable(${PROJECT_NAME} main.cpp)

target_sources(${PROJECT_NAME}
  PRIVATE
    main.cpp  
)

target_link_libraries(${PROJECT_NAME}
 PRIVATE
  spdlog::spdlog
  ${VTK_LIBRARIES})

# VTK module auto-init (needed esp. for static builds on Windows)
vtk_module_autoinit(
  TARGETS ${PROJECT_NAME}
  MODULES ${VTK_LIBRARIES}
)