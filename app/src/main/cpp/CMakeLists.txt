cmake_minimum_required(VERSION 3.22.1)
project("openusbip")

add_library(openusbip SHARED usbip_jni.c)
find_library(log-lib log)
target_link_libraries(openusbip ${log-lib})
