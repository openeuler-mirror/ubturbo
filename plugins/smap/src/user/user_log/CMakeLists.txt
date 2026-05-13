file(GLOB_RECURSE SMAP_USER_LOG_SRCS
        ${PROJECT_SOURCE_DIR}/src/user/user_log/*.c
)
add_library(user_log STATIC ${SMAP_USER_LOG_SRCS})
target_include_directories(user_log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(user_log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/smap_user_log)
target_include_directories(user_log PUBLIC ${PROJECT_3RDPARTY_SRC_DIR}/secure/libboundscheck/include)
