# - Our minimal support target is the CMake version distributed by the Ubuntu
#   LTS that we use in production, currently 24.04.
# - Our latest tested target is the CMake version distributed by Arch Linux,
#   4.1.2 at the time of writing. (TODO: Update regularly)
cmake_minimum_required(VERSION 3.28.3..4.1.2)

project(Udipe LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# TODO: Add targets + install