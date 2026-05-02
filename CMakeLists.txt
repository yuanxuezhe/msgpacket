cmake_minimum_required(VERSION 3.15)
project(msgpacket VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ================================================================
# 平台检测
# ================================================================
if(WIN32)
    set(PLATFORM_DIR "x64")
elseif(APPLE)
    set(PLATFORM_DIR "macOS")
else()
    set(PLATFORM_DIR "Lnx64")
endif()

# ================================================================
# 输出目录
#   .exe      → bin/{platform}/
#   .dll      → library/bin/{platform}/
#   .dll.a    → library/lib/{platform}/
#   .a        → library/lib/{platform}/
# ================================================================
set(BIN_DIR     ${CMAKE_SOURCE_DIR}/bin/${PLATFORM_DIR})
set(LIB_BIN_DIR ${CMAKE_SOURCE_DIR}/library/bin/${PLATFORM_DIR})
set(LIB_LIB_DIR ${CMAKE_SOURCE_DIR}/library/lib/${PLATFORM_DIR})

# 全局默认：exe → bin/，静态库 → library/lib/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${BIN_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${LIB_LIB_DIR})
foreach(CONFIG IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG} ${BIN_DIR})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG} ${LIB_LIB_DIR})
endforeach()

# ================================================================
# 源文件
# ================================================================
file(GLOB SRC_FILES "${CMAKE_SOURCE_DIR}/src/*.c")

# ================================================================
# 动态库 → library/bin/{platform}/libmsgpacket.dll
#         library/lib/{platform}/libmsgpacket.dll.a
# ================================================================
add_library(msgpacket_shared SHARED ${SRC_FILES})
set_target_properties(msgpacket_shared PROPERTIES
    OUTPUT_NAME "msgpacket"
    RUNTIME_OUTPUT_DIRECTORY ${LIB_BIN_DIR}          # .dll 放 library/bin/
    ARCHIVE_OUTPUT_DIRECTORY ${LIB_LIB_DIR}          # .dll.a 放 library/lib/
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)
# 覆盖所有构建配置
foreach(CONFIG IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(msgpacket_shared PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${CONFIG} ${LIB_BIN_DIR}
        ARCHIVE_OUTPUT_DIRECTORY_${CONFIG} ${LIB_LIB_DIR}
    )
endforeach()

target_include_directories(msgpacket_shared PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/library/include
)

# ================================================================
# 静态库 → library/lib/{platform}/libmsgpacket.a
# ================================================================
add_library(msgpacket_static STATIC ${SRC_FILES})
set_target_properties(msgpacket_static PROPERTIES
    OUTPUT_NAME "msgpacket"
    ARCHIVE_OUTPUT_DIRECTORY ${LIB_LIB_DIR}
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)
foreach(CONFIG IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(msgpacket_static PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY_${CONFIG} ${LIB_LIB_DIR}
    )
endforeach()

target_include_directories(msgpacket_static PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/library/include
)

# ================================================================
# 头文件同步到 library/include/
# ================================================================
file(GLOB HEADER_FILES "${CMAKE_SOURCE_DIR}/src/*.h")
foreach(HEADER ${HEADER_FILES})
    get_filename_component(HEADER_NAME ${HEADER} NAME)
    configure_file(${HEADER} ${CMAKE_SOURCE_DIR}/library/include/${HEADER_NAME} COPYONLY)
endforeach()

# ================================================================
# Demo — exe 继承全局 RUNTIME_OUTPUT_DIRECTORY → bin/{platform}/
# ================================================================
option(BUILD_DEMOS "Build demo applications" ON)
if(BUILD_DEMOS)
    add_subdirectory(demo/c)
    add_subdirectory(demo/cpp)
endif()
