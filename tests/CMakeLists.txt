# tests/CMakeLists.txt — 单元测试（MinUnit，零外部依赖）

if(NOT BUILD_TESTS)
    return()
endif()

file(GLOB TEST_SRC
    "${CMAKE_SOURCE_DIR}/tests/test_msgpacket.c"
    "${CMAKE_SOURCE_DIR}/src/msg_api.c"
    "${CMAKE_SOURCE_DIR}/src/msg_util.c"
    "${CMAKE_SOURCE_DIR}/src/msg_build.c"
    "${CMAKE_SOURCE_DIR}/src/msg_query.c"
)

file(GLOB PERF_SRC
    "${CMAKE_SOURCE_DIR}/tests/test_perf.c"
    "${CMAKE_SOURCE_DIR}/src/msg_api.c"
    "${CMAKE_SOURCE_DIR}/src/msg_util.c"
    "${CMAKE_SOURCE_DIR}/src/msg_build.c"
    "${CMAKE_SOURCE_DIR}/src/msg_query.c"
)

add_executable(test_msgpacket ${TEST_SRC})
add_executable(test_perf ${PERF_SRC})

foreach(target test_msgpacket test_perf)
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests
        ${LIB_INCLUDE_DIR}
    )
    target_link_libraries(${target} m)
    add_test(NAME ${target} COMMAND ${target})
endforeach()

# Performance test executable already defined above
