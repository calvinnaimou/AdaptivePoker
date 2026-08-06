cmake_minimum_required(VERSION 3.22)

project(PokerGame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Homebrew's SFML is built as shared libraries - without this, ImGui-SFML's
# CMake defaults to requesting a static SFML build and fails to find one
set(BUILD_SHARED_LIBS ON)

find_package(SFML 3 COMPONENTS Graphics Window System REQUIRED)

include(FetchContent)

# Dear ImGui has no CMakeLists.txt of its own - this just downloads the source
# and sets imgui_SOURCE_DIR, which ImGui-SFML's build then points IMGUI_DIR at
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_DIR ${imgui_SOURCE_DIR})

FetchContent_Declare(
    imgui-sfml
    GIT_REPOSITORY https://github.com/SFML/imgui-sfml.git
    GIT_TAG v3.0
)
FetchContent_MakeAvailable(imgui-sfml)

# the poker engine lives in ../engine - it has no dependency on SFML/ImGui
set(ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../engine)

add_executable(PokerGame
    src/main.cpp
    src/gameSession.cpp
    ${ENGINE_DIR}/card.cpp
    ${ENGINE_DIR}/handEval.cpp
    ${ENGINE_DIR}/calculator.cpp
    ${ENGINE_DIR}/equity.cpp
    ${ENGINE_DIR}/game.cpp
    ${ENGINE_DIR}/player.cpp
)

target_include_directories(PokerGame PRIVATE ${ENGINE_DIR})

target_link_libraries(PokerGame
    PRIVATE
    SFML::Graphics
    SFML::Window
    SFML::System
    ImGui-SFML::ImGui-SFML
)

# one-time tool: regenerates assets/cards/deck.png - run from PokerGame/ so
# the relative output path resolves (e.g. `./build/GenerateCardSheet`)
add_executable(GenerateCardSheet tools/generate_card_sheet.cpp)
target_link_libraries(GenerateCardSheet PRIVATE SFML::Graphics)