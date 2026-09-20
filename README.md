# AdaptivePoker
AdaptivePoker is a C++ poker application that evaluates hands, calculates equity, and manages full poker game sessions through a graphical interface.

## Demo
![AdaptivePoker setup screen](docs/setup.png)<br>
![AdaptivePoker table](docs/gameplay.png)

## Features
-Configurable table: 2-10 players, starting chip stack, and blind level<br>
-Mix of CPU personalities(Tight, Bluffer, Balanced, Advanced) with a randomize-and-start option<br>
-Evaluates poker hands and determines winners<br>
-Live hand odds display, showing the player's chance of ending up with each hand type<br>
-Manages players, cards, and game state across a full hand<br>
-Handles side pots for all-in situations<br>
-Includes debug and release build modes<br>
-Displays cards, chip stacks, and game state through a graphical interface

## Technologies
-C++17<br>
-CMake<br>
-SFML 3 (Graphics, Window, System)<br>
-Dear ImGui + ImGui-SFML

## Project Structure
-PokerGame/: graphical application, game session, and assets<br>
-src/: application entry point and game session glue code<br>
-assets/: card images and other visual assets<br>
-tools/: one-off asset generation utilities<br>
-engine/: cards, players, hand evaluation, equity, and game logic (no dependency on SFML/ImGui)

## Build and Run
### Requirements
-C++ compiler with C++17 support<br>
-CMake 3.22+<br>
-SFML 3 (e.g. brew install sfml on macOS)

## macOS or Linux
`cd PokerGame`<br>
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`<br>
`cmake --build build`<br>
`./build/PokerGame`

## Build Modes
### Release build:
`cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`<br>
`cmake --build build-release`

### Debug build:
`cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug`<br>
`cmake --build build-debug`

## How It Works
The project separates the poker engine from the graphical application. The engine (engine/) handles cards, players, game state, hand evaluation, and equity calculations, with no dependency on SFML or ImGui. The application (PokerGame/) links against the engine and is responsible for rendering, input, and session flow.

## What I Learned
-Structuring a larger C++ project across multiple classes<br>
-Separating application code from core game logic<br>
-Handling edge cases like side pots when players go all-in for different amounts

## Future Improvements
-Improving the UI<br>
-Improving user play/interaction flow<br>
-Additional polish and features
