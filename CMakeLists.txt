cmake_minimum_required(VERSION 3.10)
# Use vcpkg
include(UseVcpkg.cmake)

project(simple_vtk_example LANGUAGES CXX)

find_package(spdlog CONFIG REQUIRED)
find_package(VTK CONFIG REQUIRED)

add_executable(${PROJECT_NAME} main.cpp)

target_sources(${PROJECT_NAME}
  PRIVATE
    main.cpp  
)

target_link_libraries(${PROJECT_NAME}
 PRIVATE
  spdlog::spdlog
  ${VTK_LIBRARIES})

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
