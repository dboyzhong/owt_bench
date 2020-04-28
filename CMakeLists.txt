cmake_minimum_required (VERSION 2.8.4)
project (owt_test)

#SET(CMAKE_C_COMPILER "gcc-7")
#SET(CMAKE_CXX_COMPILER "g++-7")

add_definitions(-DWEBRTC_LINUX)
add_definitions(-DBOOST_LOG_DYN_LINK)
add_definitions(-DBOOST_COROUTINES_NO_DEPRECATION_WARNING)

include_directories("${PROJECT_SOURCE_DIR}/include")

set(SRC_LIST
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/processor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/EncodedVideoInput.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/YuvVideoInput.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dispatcher.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/logger.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/audioframegenerator.cpp"
)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -ggdb3 -gdwarf-2 -std=c++14")

add_library (owt STATIC IMPORTED)
set_target_properties(owt PROPERTIES IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/libs/libowt.a")

add_library (crypto STATIC IMPORTED)
set_target_properties(crypto PROPERTIES IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/libs/libcrypto.a")

add_library (ssl STATIC IMPORTED)
set_target_properties(ssl PROPERTIES IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/libs/libssl.a")

add_library (sioclient_tls STATIC IMPORTED)
set_target_properties(sioclient_tls PROPERTIES IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/libs/libsioclient_tls.a")

#add_library (owt_sdk_base STATIC IMPORTED)
#set_target_properties(owt_sdk_base PROPERTIES IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/libs/libowt_sdk_base.a")
#
#add_library (owt_sdk_conf STATIC IMPORTED)
#set_target_properties(owt_sdk_conf PROPERTIES IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/libs/libowt_sdk_conf.a")

add_executable(owt_test ${SRC_LIST})
target_link_libraries(owt_test owt sioclient_tls ssl crypto boost_system boost_coroutine boost_thread pthread dl X11)

