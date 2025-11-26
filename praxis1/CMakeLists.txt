cmake_minimum_required(VERSION 3.5)
project(RN-Praxis)
set(CMAKE_C_STANDARD 11)

# Support both C and C++ submissions: prefer C if present, otherwise fall back to C++.
set(WEB_SERVER_SOURCE "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/webserver.c")
    set(WEB_SERVER_SOURCE src/webserver.c)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/webserver.cpp")
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(WEB_SERVER_SOURCE src/webserver.cpp)
else()
    message(FATAL_ERROR "Neither src/webserver.c nor src/webserver.cpp found in ${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

add_executable(webserver ${WEB_SERVER_SOURCE})
target_compile_options(webserver PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(webserver PRIVATE -lm)

# Packaging
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_IGNORE_FILES 
    ${CMAKE_BINARY_DIR}
    /\\..*$
    \\.pdf$
    /build/
    /test/__pycache__
    \\.tar.gz$
)
set(CPACK_VERBATIM_VARIABLES YES)
include(CPack)
