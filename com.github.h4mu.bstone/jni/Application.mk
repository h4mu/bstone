
# Uncomment this if you're using STL in your project
# See CPLUSPLUS-SUPPORT.html in the NDK documentation for more information
NDK_TOOLCHAIN_VERSION := 4.9
APP_STL := c++_static 
APP_CPPFLAGS += --std=c++11 -fexceptions
APP_ABI := armeabi armeabi-v7a x86
