cmake_minimum_required(VERSION 3.7)

project(apromise
  VERSION 0.1.0
  LANGUAGES CXX
)

find_package(cupcake 0.4.0 REQUIRED)

cupcake_project()

cupcake_add_library(apromise)

cupcake_add_executable(apromise)
target_link_libraries(${this} PRIVATE apromise.libapromise)

cupcake_add_tests()

cupcake_install_project()
cupcake_install_cpp_info()
