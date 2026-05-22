cmake_minimum_required(VERSION 3.15)
project(msgpacket VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ================================================================
# 平台检测
# ================================================================
if(WIN32)
    set(PLATFORM_DIR "x64")
    set(SHARED_EXT "dll")
    set(IMPORT_LIB_EXT "lib")
elseif(APPLE)
    set(PLATFORM_DIR "macOS")
    set(SHARED_EXT "dylib")
    set(IMPORT_LIB_EXT "dylib")
else()
    set(PLATFORM_DIR "Lnx64")
    set(SHARED_EXT "so")
    set(IMPORT_LIB_EXT "so")
endif()

# ================================================================
# 输出目录
#   bin/{platform}/          — 可执行文件 + 动态库 (.so/.dll)
#   library/include/         — 头文件
#   library/lib/{platform}/   — 静态库 + 导入库
# ================================================================
set(BIN_DIR         ${CMAKE_SOURCE_DIR}/bin/${PLATFORM_DIR})
set(LIB_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/library/include)
set(LIB_LIB_DIR     ${CMAKE_SOURCE_DIR}/library/lib/${PLATFORM_DIR})

# 可执行文件 → bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${BIN_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${LIB_LIB_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${BIN_DIR})
foreach(CONFIG IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG} ${BIN_DIR})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG} ${LIB_LIB_DIR})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG} ${BIN_DIR})
endforeach()

# ================================================================
# 头文件
# ================================================================
file(GLOB HEADER_FILES "${CMAKE_SOURCE_DIR}/src/*.h")

# ================================================================
# Sanitizer 选项（ASAN / LSAN）— 提前定义以便在各处使用
# ================================================================
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_LSAN "Enable LeakSanitizer (requires ASAN)" OFF)

# ASAN 配置
if(ENABLE_ASAN)
    set(ASAN_EXE_FLAGS -fsanitize=address -fno-omit-frame-pointer)
    set(ASAN_SHARED_FLAGS -fsanitize=address)
    set(ASAN_LINK_FLAGS -fsanitize=address)
    if(ENABLE_LSAN)
        list(APPEND ASAN_EXE_FLAGS -fsanitize=leak)
        list(APPEND ASAN_LINK_FLAGS -fsanitize=leak)
    endif()
endif()

# ================================================================
# 动态库
# ================================================================
add_library(msgpacket_shared SHARED ${CMAKE_SOURCE_DIR}/src/msg_api.c
                                         ${CMAKE_SOURCE_DIR}/src/msg_util.c
                                         ${CMAKE_SOURCE_DIR}/src/msg_build.c
                                         ${CMAKE_SOURCE_DIR}/src/msg_query.c)
set_target_properties(msgpacket_shared PROPERTIES
    OUTPUT_NAME "msgpacket"
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(msgpacket_shared PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include/msgpacket>
)

# ================================================================
# 静态库
# ================================================================
add_library(msgpacket_static STATIC ${CMAKE_SOURCE_DIR}/src/msg_api.c
                                           ${CMAKE_SOURCE_DIR}/src/msg_util.c
                                           ${CMAKE_SOURCE_DIR}/src/msg_build.c
                                           ${CMAKE_SOURCE_DIR}/src/msg_query.c)
set_target_properties(msgpacket_static PROPERTIES
    OUTPUT_NAME "msgpacket"
    VERSION     ${PROJECT_VERSION}
    SOVERSION   1
)

target_include_directories(msgpacket_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include/msgpacket>
)

# ================================================================
# 头文件 → library/include/
# ================================================================
foreach(HEADER ${HEADER_FILES})
    get_filename_component(HEADER_NAME ${HEADER} NAME)
    configure_file(${HEADER} ${LIB_INCLUDE_DIR}/${HEADER_NAME} COPYONLY)
endforeach()

# ================================================================
# Sanitizer 标志（为所有可执行目标应用）
# 注意：必须在 add_subdirectory 之前设置，这样子目录中的目标会继承这些选项
# ================================================================
if(ENABLE_ASAN)
    add_compile_options(${ASAN_EXE_FLAGS})
    add_link_options(${ASAN_LINK_FLAGS})
endif()

# ================================================================
# Demo
# ================================================================
option(BUILD_DEMOS "Build demo applications" ON)
if(BUILD_DEMOS)
    add_subdirectory(demo/c)
    add_subdirectory(demo/cpp)
endif()

# ================================================================
# 单元测试（MinUnit — 零依赖，跨平台）
# ================================================================
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ================================================================
# install — 头文件 + 动态库 + 静态库 → library/
# ================================================================
include(GNUInstallDirs)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/msgpacket"
    FILES_MATCHING PATTERN "*.h")

install(TARGETS msgpacket_shared msgpacket_static
    EXPORT msgpacketTargets
    RUNTIME       DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY       DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE       DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/msgpacket")

install(EXPORT msgpacketTargets
    FILE msgpacketTargets.cmake
    NAMESPACE msgpacket::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/msgpacket")

# bin/ 目录运行时副本（构建完成后自动复制）
add_custom_command(TARGET msgpacket_shared POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:msgpacket_shared>
        ${BIN_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}msgpacket.${SHARED_EXT}
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:msgpacket_static>
        ${LIB_LIB_DIR}/libmsgpacket.a
    COMMENT "Copying built artifacts to bin/ and library/")
