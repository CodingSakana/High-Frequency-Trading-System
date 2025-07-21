# /common/CMakeLists.txt

file(GLOB SOURCES "*.cpp")

add_library(libcommon STATIC ${SOURCES})

set(LIBS "")

list(APPEND LIBS libcommon)
list(APPEND LIBS pthread)

add_executable(thread_example thread_example.cpp)
target_link_libraries(thread_example PRIVATE ${LIBS})

add_executable(mem_pool_example mem_pool_example.cpp)
target_link_libraries(mem_pool_example PRIVATE ${LIBS})

add_executable(lf_queue_example lf_queue_example.cpp)
target_link_libraries(lf_queue_example PRIVATE ${LIBS})

add_executable(logging_example logging_example.cpp)
target_link_libraries(logging_example PRIVATE ${LIBS})

add_executable(socket_example socket_example.cpp)
target_link_libraries(socket_example PRIVATE ${LIBS})
