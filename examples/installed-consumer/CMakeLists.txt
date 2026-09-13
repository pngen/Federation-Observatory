cmake_minimum_required(VERSION 3.20)
project(FederationObservatoryConsumer LANGUAGES CXX)

# An independent downstream consumer. It configures against an INSTALLED Federation
# Observatory and never references the source tree or its build directory.
#
#   cmake -S examples/installed-consumer -B <build> -DCMAKE_PREFIX_PATH=<install-prefix>
#   cmake --build <build>
#   <build>/fo-consumer

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(FederationObservatory CONFIG REQUIRED)

add_executable(fo-consumer main.cpp)
target_link_libraries(fo-consumer PRIVATE SummonSoftwareLabs::FederationObservatory)

message(STATUS "consumer: using Federation Observatory ${FederationObservatory_VERSION}")
