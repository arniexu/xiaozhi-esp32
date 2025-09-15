# 递归收集所有源文件
file(GLOB_RECURSE CURL_SOURCES 
    "lib/*.c"
)

# 递归收集所有头文件目录
file(GLOB_RECURSE ALL_HEADERS "lib/*.h" "include/*.h")
set(CURL_INCLUDE_DIRS "")
foreach(header ${ALL_HEADERS})
    get_filename_component(header_dir ${header} DIRECTORY)
    list(APPEND CURL_INCLUDE_DIRS ${header_dir})
endforeach()
list(REMOVE_DUPLICATES CURL_INCLUDE_DIRS)

# 将绝对路径转为相对路径
set(CURL_INCLUDE_DIRS_REL "")
foreach(dir ${CURL_INCLUDE_DIRS})
    file(RELATIVE_PATH rel_dir ${CMAKE_CURRENT_SOURCE_DIR} ${dir})
    list(APPEND CURL_INCLUDE_DIRS_REL ${rel_dir})
endforeach()

idf_component_register(
    SRCS ${CURL_SOURCES}
    INCLUDE_DIRS "include" ${CURL_INCLUDE_DIRS_REL}
)

# 添加编译定义，同步 curl_config.h 的所有宏
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAVE_CONFIG_H=1
    BUILDING_LIBCURL=1
)
