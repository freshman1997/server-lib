cmake_minimum_required(VERSION 3.12)
project(plugins)

add_library(HelloWorld SHARED ./helloworld/helloworld.cpp)
target_link_libraries(HelloWorld PUBLIC Core)

set(targets HelloWorld)

foreach(target ${targets})
    set_target_properties(${target} PROPERTIES 
        OUTPUT_NAME HelloWorld 
        PREFIX "" 
        SUFFIX ".plugin"
    )
endforeach(target ${targets})

