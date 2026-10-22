# core：通信库（零 Qt）
add_library(relayer_core STATIC src/placeholder.cpp)
target_include_directories(relayer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(relayer_core PUBLIC relayer_filestore PRIVATE relayer_platform)
relayer_set_warnings(relayer_core)
