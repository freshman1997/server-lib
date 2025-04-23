cmake_minimum_required(VERSION 3.12)
project(plugins)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

add_library(HelloWorld SHARED ./helloworld/helloworld.cpp)
target_link_libraries(HelloWorld PRIVATE Core)

set(targets HelloWorld)

foreach(target ${targets})
    set_target_properties(${target} PROPERTIES 
        OUTPUT_NAME ${target} 
        PREFIX "" 
        SUFFIX ".plugin"
    )
endforeach(target ${targets})

