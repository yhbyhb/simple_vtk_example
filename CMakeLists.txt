cmake_minimum_required(VERSION 3.28)
# Use vcpkg
include(UseVcpkg.cmake)

# Specify C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

project(simple_vtk_dicom LANGUAGES CXX)

find_package(dicom CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(VTK REQUIRED)

add_executable(${PROJECT_NAME} main.cpp)

target_sources(${PROJECT_NAME}
  PRIVATE
    main.cpp
)

target_link_libraries(${PROJECT_NAME}
  PRIVATE
    ${VTK_LIBRARIES}
    spdlog::spdlog
    VTK::DICOM
)

# VTK module auto-init (needed esp. for static builds on Windows)
vtk_module_autoinit(
  TARGETS ${PROJECT_NAME}
  MODULES ${VTK_LIBRARIES}
)