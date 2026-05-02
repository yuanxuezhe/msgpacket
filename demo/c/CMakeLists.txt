if(BUILD_DEMOS)
    add_executable(demo_builder demo_builder.c)
    target_link_libraries(demo_builder msgpacket_shared msgpacket_static)

    add_executable(demo_parser demo_parser.c)
    target_link_libraries(demo_parser msgpacket_shared msgpacket_static)

    add_executable(demo_full_cycle demo_full_cycle.c)
    target_link_libraries(demo_full_cycle msgpacket_shared msgpacket_static)
endif()
