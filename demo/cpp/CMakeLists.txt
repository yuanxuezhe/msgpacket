if(BUILD_DEMOS)
    # C++ demo requires C++11 or later
    set(CMAKE_CXX_STANDARD 11)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    add_executable(demo_cpp demo.cpp)
    target_link_libraries(demo_cpp msgpacket_shared)
endif()
