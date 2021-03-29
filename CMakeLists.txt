cmake_minimum_required(VERSION 3.0)
project(tcploadgen CXX)

add_subdirectory(libixxx)
add_subdirectory(libixxxutil)


set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)


add_executable(tcploadgen
    main.cc
    config.cc
    receiver.cc
    client.cc
    )
set_property(TARGET tcploadgen PROPERTY INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tomlplusplus/include
    ${CMAKE_SOURCE_DIR}/libixxx
    ${CMAKE_SOURCE_DIR}/libixxxutil
    )
target_link_libraries(tcploadgen
    ixxxutil_static
    ixxx_static
    Threads::Threads
    )

# add_executable(test_toml
#     test_toml.cc
#     )
# set_property(TARGET test_toml PROPERTY INCLUDE_DIRECTORIES
#     ${CMAKE_SOURCE_DIR}/tomlplusplus/include
#     )
