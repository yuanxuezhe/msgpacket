# tests/CMakeLists.txt — 单元测试（MinUnit，零外部依赖）

if(NOT BUILD_TESTS)
    return()
endif()

file(GLOB TEST_SRC
    "${CMAKE_SOURCE_DIR}/tests/test_msgpacket.c"
    "${CMAKE_SOURCE_DIR}/src/msg_api.c"
    "${CMAKE_SOURCE_DIR}/src/msg_util.c"
)

add_executable(test_msgpacket ${TEST_SRC})

target_include_directories(test_msgpacket PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests
    ${LIB_INCLUDE_DIR}
)

# 链接 math 库（部分平台需要）
target_link_libraries(test_msgpacket m)

add_test(NAME test_msgpacket COMMAND test_msgpacket)
