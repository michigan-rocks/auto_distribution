cmake_minimum_required(VERSION 3.20)

option(BUILD_PRODUCER "Build Producer" ON)
option(BUILD_INTERNAL "Build Dispatcher" ON)
option(BUILD_WORKERS "Build Worker" ON)
option(BUILD_ROUND_ROBIN "Build Round Robin dispatcher" ON)

project(optotech LANGUAGES CXX)

include(CTest)

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

message(STATUS "SimpleAmqpClient include: ${SIMPLE_AMQP_INCLUDE_DIR}")
message(STATUS "SimpleAmqpClient library: ${SIMPLE_AMQP_LIBRARY}")

##продюсер

if(BUILD_PRODUCER)
    find_package(nlohmann_json CONFIG REQUIRED)

    add_executable(producer
        producer/src/main.cpp
        producer/src/task.cpp
    )

    target_include_directories(producer PRIVATE
        "${SIMPLE_AMQP_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
    )

    target_link_libraries(producer PRIVATE
        "${SIMPLE_AMQP_LIBRARY}"
        rabbitmq::rabbitmq
        Boost::chrono
        Boost::system
        nlohmann_json::nlohmann_json
    )
endif()

if(BUILD_INTERNAL)
    find_package(nlohmann_json CONFIG REQUIRED)

    add_executable(internal
        internal/src/main.cpp
    )

    target_include_directories(internal PRIVATE
        "${SIMPLE_AMQP_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
    )

    target_link_libraries(internal PRIVATE
        "${SIMPLE_AMQP_LIBRARY}"
        rabbitmq::rabbitmq
        Boost::chrono
        Boost::system
        nlohmann_json::nlohmann_json
    )
endif()

if(BUILD_ROUND_ROBIN)
    add_executable(round_robin
        round_robin/src/main.cpp
    )

    target_include_directories(round_robin PRIVATE
        "${SIMPLE_AMQP_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
    )

    target_link_libraries(round_robin PRIVATE
        "${SIMPLE_AMQP_LIBRARY}"
        rabbitmq::rabbitmq
        Boost::chrono
        Boost::system
    )
endif()

if(BUILD_WORKERS)
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(prometheus-cpp CONFIG REQUIRED)

    add_executable(worker
        workers/src/main.cpp
    )

    target_include_directories(worker PRIVATE
        "${SIMPLE_AMQP_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
    )

    target_link_libraries(worker PRIVATE
        "${SIMPLE_AMQP_LIBRARY}"
        rabbitmq::rabbitmq
        Boost::chrono
        Boost::system
        nlohmann_json::nlohmann_json
        prometheus-cpp::pull
    )
endif()

if(BUILD_INTERNAL AND BUILD_TESTING)
    add_executable(test_adaptive_scheduler
        internal/tests/test_adaptive_scheduler.cpp
    )
    target_include_directories(test_adaptive_scheduler PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/internal/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
    )
    add_test(NAME adaptive_scheduler COMMAND test_adaptive_scheduler)
endif()

if(BUILD_ROUND_ROBIN AND BUILD_TESTING)
    add_executable(test_round_robin_scheduler
        round_robin/tests/test_round_robin_scheduler.cpp
    )
    target_include_directories(test_round_robin_scheduler PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/round_robin/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
    )
    add_test(NAME round_robin_scheduler COMMAND test_round_robin_scheduler)
endif()
