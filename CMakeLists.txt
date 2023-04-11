cmake_minimum_required(VERSION 3.7)

project(promises
  VERSION 0.1.0
  LANGUAGES CXX
)

find_package(cupcake 0.1.0 REQUIRED)

cupcake_project()

cupcake_add_library(promises)

cupcake_add_executable(promises)
target_link_libraries(${this} PRIVATE promises::libpromises)

cupcake_add_tests()

cupcake_install_project()
cupcake_install_cpp_info()
