cmake_minimum_required(VERSION 3.20)
project(sh3emu_guest C)

include(VeloCE)

velo_add_executable(netdial netdial.c)
velo_add_executable(nettest nettest.c)
target_link_libraries(nettest PRIVATE velo::winsock)
