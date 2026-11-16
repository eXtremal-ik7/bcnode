cxxpm_add_package(gtest default)
include(GoogleTest)

# One executable to a coin: the coin's code is built under -DBC with its BC/ headers, as the node's
macro(add_bc_unittest NAME)
  string(TOLOWER ${NAME} NAME_LOWER_CASE)
  set(UNITTEST_TARGET bcunittest-${NAME_LOWER_CASE})
  add_executable(${UNITTEST_TARGET} ${ARGN})
  target_include_directories(${UNITTEST_TARGET} PUBLIC ${CMAKE_BINARY_DIR}/${NAME})
  target_compile_definitions(${UNITTEST_TARGET} PUBLIC -DBC=${NAME})
  target_link_libraries(${UNITTEST_TARGET} ${GTEST_LIBRARIES} ${COMMON_LIBRARIES})
  gtest_discover_tests(${UNITTEST_TARGET} DISCOVERY_MODE PRE_TEST)
  # As for the node targets: the DLLs of shared packages next to the executable
  if (WIN32)
    add_custom_command(TARGET ${UNITTEST_TARGET} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:${UNITTEST_TARGET}> $<TARGET_FILE_DIR:${UNITTEST_TARGET}>
      COMMAND_EXPAND_LISTS
    )
  endif()
endmacro()

add_bc_unittest(BTC
  bip34Test.cpp
  fastpathTest.cpp
  localSpendTest.cpp
  outputInfoTest.cpp
  spendBuilder.cpp
  ${CMAKE_SOURCE_DIR}/BTC/policy.cpp
  ${CMAKE_SOURCE_DIR}/BTC/proto.cpp
  ${CMAKE_SOURCE_DIR}/BTC/script.cpp
  ${CMAKE_SOURCE_DIR}/BTC/scriptChecks.cpp
  ${CMAKE_SOURCE_DIR}/BTC/scriptError.cpp
  ${CMAKE_SOURCE_DIR}/BTC/sigCheck.cpp
  ${CMAKE_SOURCE_DIR}/BTC/sighash.cpp
)
