cmake_minimum_required(VERSION 3.23)
project(untitled)

#set(CMAKE_CXX_STANDARD 14)
#set(CMAKE_C_STANDARD 11)

add_executable(untitled main.c mongoose.c)
target_link_libraries(untitled pthread m)