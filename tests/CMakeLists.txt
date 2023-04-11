cupcake_find_package(doctest 2.4 PRIVATE)

cupcake_add_test_executable(promises promises.cpp)
target_link_libraries(${this}
  promises::libpromises
  doctest::doctest
)
