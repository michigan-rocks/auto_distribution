cmake_minimum_required(VERSION 3.20)

project(optotech LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_path(SIMPLE_AMQP_INCLUDE_DIR
    NAMES SimpleAmqpClient/SimpleAmqpClient.h
)

find_library(SIMPLE_AMQP_LIBRARY
    NAMES SimpleAmqpClient.7 SimpleAmqpClient
)

if(NOT SIMPLE_AMQP_INCLUDE_DIR OR NOT SIMPLE_AMQP_LIBRARY)
    message(FATAL_ERROR "SimpleAmqpClient not found")
endif()

find_package(rabbitmq-c CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS chrono system)
find_package(nlohmann_json CONFIG REQUIRED)

message(STATUS "SimpleAmqpClient include: ${SIMPLE_AMQP_INCLUDE_DIR}")
message(STATUS "SimpleAmqpClient library: ${SIMPLE_AMQP_LIBRARY}")

##продюсер
add_executable(producer
    producer/src/main.cpp
    producer/src/task.cpp
)

target_include_directories(producer PRIVATE
    "${SIMPLE_AMQP_INCLUDE_DIR}"
)

target_link_libraries(producer PRIVATE
    "${SIMPLE_AMQP_LIBRARY}"
    rabbitmq::rabbitmq
    Boost::chrono
    Boost::system
    nlohmann_json::nlohmann_json
)
