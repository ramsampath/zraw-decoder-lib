cmake_minimum_required(VERSION 3.15)

project(zrawdecoderlib LANGUAGES CXX)

# Set output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(OPENSSL_ROOT_DIR "F:/OpenSSL-Win64") # Windows example

set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT_DIR}/lib/VC/x64/MTd/libcrypto.lib")
set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT_DIR}/lib/VC/x64/MTd/libssl.lib")



# Define static library

add_library(zrawdecoderlib SHARED)


# Set source files
set(SOURCES
    src/interface.cpp
    src/Tools.cpp
    src/ZRawFrameContainerParserSingletone.cpp
    src/ZRawFrameDecompressorSingletone.cpp
    src/ZRawFramePreProcessorSingletone.cpp
)
set(HEADERS
    include/byteswap.hpp
    include/libzraw.h
    src/BitReader.hpp
    src/IConsoleOutput.hpp
    src/Tools.hpp
    src/ZRawFrame.hpp
    src/ZRawFrameContainerParserSingletone.hpp
    src/ZRawFrameDecompressorSingletone.hpp
    src/ZRawFramePreProcessorSingletone.hpp
    src/ZRawImageBlockLine.hpp
    src/ZRawImageLineBlockReader.hpp
    src/ZRawLowLevelStructures.hpp
)



# Add sources to the library
target_sources(zrawdecoderlib PRIVATE ${SOURCES} ${HEADERS})

# Include directories
target_include_directories(zrawdecoderlib PRIVATE ../src ../include)

# Compiler settings
if(MSVC)
    target_compile_options(zrawdecoderlib PRIVATE /W3 /permissive- /MP)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Set configurations
foreach(config Debug Release)
    set_property(TARGET zrawdecoderlib APPEND PROPERTY COMPILE_DEFINITIONS $<$<CONFIG:${config}>:${config}>)
endforeach()

# OpenSSL for x64 builds
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    find_package(OpenSSL REQUIRED)
    target_include_directories(zrawdecoderlib PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_link_libraries(zrawdecoderlib PRIVATE ${OPENSSL_CRYPTO_LIBRARY})
endif()

# Post-build step to copy library to build folder
add_custom_command(TARGET zrawdecoderlib POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/../build
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:zrawdecoderlib> ${CMAKE_SOURCE_DIR}/../build/
)

# Set target name
set_target_properties(zrawdecoderlib PROPERTIES OUTPUT_NAME "libzraw")

# Install target
install(TARGETS zrawdecoderlib
    RUNTIME DESTINATION lib
)
install(DIRECTORY include DESTINATION .)

# Install OpenSSL dependencies
install(FILES ${OPENSSL_CRYPTO_LIBRARY} ${OPENSSL_SSL_LIBRARY}
    DESTINATION lib
)

install(DIRECTORY ${OPENSSL_INCLUDE_DIR} DESTINATION .)

message(STATUS "OpenSSL found at: ${OPENSSL_ROOT_DIR}")
