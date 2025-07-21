file(GLOB SOURCES "*/*.cpp")

add_library(libexchange STATIC ${SOURCES})
target_include_directories(libexchange PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}
)