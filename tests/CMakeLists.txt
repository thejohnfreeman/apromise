cupcake_find_package(doctest 2.4 PRIVATE)

cupcake_add_test_executable(apromise apromise.cpp)
target_link_libraries(${this}
  apromise::libapromise
  doctest::doctest
)
