cmake_minimum_required(VERSION 3.16)
project(FtpCompatTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_subdirectory(.. QFtpCompat)

qt_add_executable(FtpCompatTest
    main.cpp
)

target_link_libraries(FtpCompatTest
    PRIVATE
        Qt6::Core
        Qt6::Network
        QFtpCompat
)
