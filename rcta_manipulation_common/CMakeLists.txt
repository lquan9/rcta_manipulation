cmake_minimum_required(VERSION 2.8.12)
project(rcta_manipulation_common)

find_package(catkin REQUIRED)

catkin_package(INCLUDE_DIRS include)

install(
    DIRECTORY include/rcta_manipulation_common/
    DESTINATION ${CATKIN_PACKAGE_INCLUDE_DESTINATION})
