# tests/CMakeLists.txt — cmocka 单元测试
if(NOT BUILD_TESTS)
    return()
endif()

file(GLOB TEST_SRC
    "${CMAKE_SOURCE_DIR}/tests/*.c"
    "${CMAKE_SOURCE_DIR}/src/msg_api.c"
    "${CMAKE_SOURCE_DIR}/src/msg_util.c"
)

find_package(cmocka QUIET)
if(NOT cmocka_FOUND)
    # 使用 ExternalProject 编译的 cmocka
    include_directories(${CMOCKA_INSTALL_DIR}/include)
    link_directories(${CMOCKA_INSTALL_DIR}/lib)
endif()

add_executable(test_msgpacket ${TEST_SRC})
target_include_directories(test_msgpacket PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${LIB_INCLUDE_DIR}
)

if(cmocka_FOUND)
    target_link_libraries(test_msgpacket cmocka::cmocka)
else()
    target_link_libraries(test_msgpacket ${CMOCKA_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}cmocka${CMAKE_STATIC_LIBRARY_SUFFIX}
                                   ${CMOCKA_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}cmocka_static${CMAKE_STATIC_LIBRARY_SUFFIX})
endif()

add_test(NAME test_msgpacket COMMAND test_msgpacket)
