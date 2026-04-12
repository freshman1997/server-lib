cmake_minimum_required(VERSION 3.12)
project(plugins)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(HelloWorld SHARED ./helloworld/helloworld.cpp)
target_link_libraries(HelloWorld PRIVATE App PluginCore)

set(targets HelloWorld)

foreach(target ${targets})
    set_target_properties(${target} PROPERTIES 
        OUTPUT_NAME ${target} 
        PREFIX "" 
        SUFFIX ".plugin"
    )
endforeach(target ${targets})

