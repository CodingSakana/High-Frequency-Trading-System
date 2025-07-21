file(GLOB SOURCES "*/*.cpp")

add_library(libtrading STATIC ${SOURCES})
target_include_directories(libtrading PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}
)
