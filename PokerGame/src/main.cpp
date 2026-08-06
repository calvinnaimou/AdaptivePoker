#include <SFML/Graphics.hpp>
#include <imgui-SFML.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "card.h"
#include "game.h"
#include "gameSession.h"
#include "player.h"

enum class AppMode {
    Welcome,
    Setup,
    Game
};

namespace {
    std::mt19937 uiRng{std::random_device{}()};

    constexpr int MIN_BUY_IN = 100;
    constexpr int MAX_BUY_IN = 1000;
    constexpr int DEFAULT_BUY_IN = MAX_BUY_IN;

    struct BlindLevel{
        const char* label;
        int smallBlind;
        int bigBlind;
    };

    //offered blind levels - chosen in Setup, applied to Player::bigBlind and
    //the Game constructor when "Start Game" is pressed
    constexpr std::array<BlindLevel, 3> blindLevels{{
        {"1/2", 1, 2},
        {"2/5", 2, 5},
        {"5/10", 5, 10}
    }};

    //fixed design-space canvas everything is laid out in; toScreen() maps it
    //into whatever the actual window size is. Sized to comfortably fit a
    //10-seat table - see computeSeatPositions/computeSeatLayout for the math
    constexpr float DESIGN_WIDTH = 1280.f;
    constexpr float DESIGN_HEIGHT = 960.f;
    constexpr float TABLE_CENTER_X = 640.f;
    constexpr float TABLE_CENTER_Y = 480.f;
    constexpr float SEAT_FONT_SCALE = 1.15f;

    //same pool cli/poker.cpp uses - showing a CPU's actual type as its name
    //would reveal its strategy, so opponents are anonymized with these instead
    std::vector<std::string> cpuNames = {
        "ChipBot7", "FoldMaster22", "RiverRat404", "BluffBot13", "AllInAndy99",
        "CallStation8", "RaiseRaptor77", "PocketPair21", "AceHunter11", "BigBlindBob25"
    };

    //true if name meets the username rules: 3-15 characters, starts with a letter, no whitespace
    bool isValidUsername(const std::string& name) {
        if (name.size() < 3 || name.size() > 15) {
            return false;
        }
        if (!std::isalpha(static_cast<unsigned char>(name[0]))) {
            return false;
        }
        for (char c : name) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                return false;
            }
        }
        return true;
    }

    //a stadium shape (a rectangle with semicircular end caps) sized width x
    //height, with its top-left corner at the origin - move it with setPosition afterward
    sf::ConvexShape makeStadium(float width, float height) {
        constexpr int POINTS_PER_CAP = 20;
        constexpr float PI = 3.14159265f;

        float radius = height / 2.f;

        sf::ConvexShape shape(POINTS_PER_CAP * 2);

        //right cap sweeps from -90 to +90 degrees around its center
        sf::Vector2f rightCenter(width - radius, radius);
        for (int i = 0; i < POINTS_PER_CAP; ++i) {
            float angle = (-90.f + 180.f * i / (POINTS_PER_CAP - 1)) * PI / 180.f;
            shape.setPoint(
                static_cast<std::size_t>(i),
                rightCenter + sf::Vector2f(radius * std::cos(angle), radius * std::sin(angle))
            );
        }

        //left cap sweeps from 90 to 270 degrees around its center
        sf::Vector2f leftCenter(radius, radius);
        for (int i = 0; i < POINTS_PER_CAP; ++i) {
            float angle = (90.f + 180.f * i / (POINTS_PER_CAP - 1)) * PI / 180.f;
            shape.setPoint(
                static_cast<std::size_t>(POINTS_PER_CAP + i),
                leftCenter + sf::Vector2f(radius * std::cos(angle), radius * std::sin(angle))
            );
        }

        return shape;
    }

    //numChips formatted with thousands separators, e.g. 12500 -> "12,500"
    std::string formatChips(int numChips) {
        std::string digits = std::to_string(numChips);
        std::string result;
        int count = 0;
        for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
            if (count != 0 && count % 3 == 0) {
                result.push_back(',');
            }
            result.push_back(*it);
            ++count;
        }
        std::reverse(result.begin(), result.end());
        return result;
    }

    //"<chips> chips (<bb> BB)" for a player with numChips chips
    std::string formatStack(int numChips) {
        double bb = static_cast<double>(numChips) / Player::bigBlind;
        char bbBuffer[32];
        std::snprintf(bbBuffer, sizeof(bbBuffer), "%.1f", bb);
        return formatChips(numChips) + " chips (" + bbBuffer + " BB)";
    }

    //display names for the odds panel, in HandCategory's own enum order
    //(highCard..royalFlush) - GameSnapshot::handOdds is indexed the same way
    constexpr const char* HAND_CATEGORY_NAMES[] = {
        "High Card", "Pair", "Two Pair", "Trips", "Straight",
        "Flush", "Full House", "Quads", "Straight Flush", "Royal Flush"
    };

    //" (Dealer)"/" (Small Blind)"/" (Big Blind)" for seatIndex's role this
    //hand, or "" if it holds none - mirrors the blind-seat selection in
    //Game::preFlop() exactly. Heads-up has no separate Dealer tag since the
    //dealer and small blind are the same seat, so that seat is labeled
    //Small Blind only
    std::string seatPositionLabel(std::size_t seatIndex, int dealer, std::size_t numPlayers) {
        std::size_t dealerIndex = static_cast<std::size_t>(dealer);
        std::size_t sbIndex = numPlayers > 2 ? (dealerIndex + 1) % numPlayers : dealerIndex;
        std::size_t bbIndex = numPlayers > 2 ? (dealerIndex + 2) % numPlayers
                                             : (dealerIndex + 1) % numPlayers;

        if (seatIndex == sbIndex) {
            return " (Small Blind)";
        }
        if (seatIndex == bbIndex) {
            return " (Big Blind)";
        }
        if (numPlayers > 2 && seatIndex == dealerIndex) {
            return " (Dealer)";
        }
        return "";
    }

    //cell size and grid layout of assets/cards/deck.png - matches
    //PokerGame/tools/generate_card_sheet.cpp exactly: column = rank - 2,
    //row = suit (SPADES=0, CLUBS=1, DIAMONDS=2, HEARTS=3)
    constexpr float CARD_SHEET_CELL_WIDTH = 300.f;
    constexpr float CARD_SHEET_CELL_HEIGHT = 420.f;

    //the sub-rect of the card sheet texture for the given card
    sf::IntRect cardTextureRect(const Card& card) {
        int col = static_cast<int>(card.rank) - static_cast<int>(Rank::TWO);
        int row = static_cast<int>(card.suit);
        return sf::IntRect(
            {static_cast<int>(col * CARD_SHEET_CELL_WIDTH), static_cast<int>(row * CARD_SHEET_CELL_HEIGHT)},
            {static_cast<int>(CARD_SHEET_CELL_WIDTH), static_cast<int>(CARD_SHEET_CELL_HEIGHT)}
        );
    }

    //loads assets/cards/deck.png into sheet, trying every plausible working
    //directory the binary might be launched from (repo root, PokerGame/, or
    //build/, in that order) - sf::Texture resolves its path relative to the
    //current working directory, not the executable's location, so launching
    //from the "wrong" directory would otherwise silently draw no card faces
    //at all instead of failing loudly, an easy trap to fall into
    bool loadCardSheet(sf::Texture& sheet) {
        static constexpr const char* candidates[] = {
            "assets/cards/deck.png",           //cwd == PokerGame/
            "PokerGame/assets/cards/deck.png", //cwd == repo root
            "../assets/cards/deck.png"         //cwd == PokerGame/build/
        };

        for (const char* path : candidates) {
            if (sheet.loadFromFile(path)) {
                return true;
            }
        }
        return false;
    }

    //draws card face-up at the given design-space position/size using sheet,
    //or does nothing if sheet failed to load
    void drawCardFace(sf::RenderWindow& window, const sf::Texture& sheet, bool sheetLoaded,
                       const Card& card, sf::Vector2f designPos, sf::Vector2f designSize) {
        if (!sheetLoaded) {
            return;
        }

        sf::Sprite sprite(sheet, cardTextureRect(card));
        sprite.setPosition(designPos);
        sprite.setScale({designSize.x / CARD_SHEET_CELL_WIDTH, designSize.y / CARD_SHEET_CELL_HEIGHT});
        window.draw(sprite);
    }

    //the display color for a seat's most recent action label - every action
    //shares the same color now, so this no longer needs to switch on which one it is
    ImVec4 actionColor(Action) {
        return {0.90f, 0.25f, 0.25f, 1.f};   //red
    }

    //pos scaled from the fixed 1280x960 design space into actual window
    //pixels, so ImGui widgets track the same stretch SFML's default view
    //already applies to regular window.draw calls
    ImVec2 toScreen(sf::Vector2f pos, sf::Vector2f scale) {
        return {pos.x * scale.x, pos.y * scale.y};
    }

    //one seat position per player, spaced evenly around an ellipse inscribed
    //in the table, starting at the bottom (the human's seat, players[0]) and
    //going clockwise. ellipseB is deliberately close to ellipseA (a
    //near-circular ellipse) because an ellipse's arc length per degree is
    //smallest near its horizontal ends - with 10 seats, a much flatter oval
    //(like a real table's silhouette) packs the left/right seats too close
    //together for their labels to fit without overlapping
    std::vector<sf::Vector2f> computeSeatPositions(std::size_t seatCount) {
        std::vector<sf::Vector2f> positions;
        sf::Vector2f tableCenter(TABLE_CENTER_X, TABLE_CENTER_Y);
        float ellipseA = 400.f;
        float ellipseB = 280.f;

        for (std::size_t i = 0; i < seatCount; ++i) {
            float angleDeg = 90.f + 360.f * static_cast<float>(i) / static_cast<float>(seatCount);
            float angleRad = angleDeg * 3.14159265f / 180.f;
            positions.push_back({
                tableCenter.x + ellipseA * std::cos(angleRad),
                tableCenter.y + ellipseB * std::sin(angleRad)
            });
        }

        return positions;
    }

    struct SeatLayout {
        float cardTopY;
        float textTopY;
        float cardWidth;
        float cardHeight;
        float halfGap;
        int textAnchor; //-1 = grows left, 0 = centered, +1 = grows right
    };

    //where a seat's cards and name/chip text should sit - cards sit toward
    //the table center (near the board), and name/chip text sits on the
    //outer side, toward the rail, for every seat regardless of whether it's
    //above or below center. Seats on the left/right thirds of the table
    //grow their text outward (away from the table's horizontal center)
    //instead of centering it, so neighboring seats' labels don't grow
    //toward each other and collide - only top/bottom seats stay centered
    SeatLayout computeSeatLayout(float seatX, float seatY, bool isHuman, float textBlockHeight) {
        float cardWidth = isHuman ? 68.f : 34.f;
        float cardHeight = isHuman ? 98.f : 48.f;
        float halfGap = isHuman ? 9.f : 5.f;

        //+1 when the outer edge is below the seat (toward larger y), -1 when above
        float dirToEdge = (seatY < TABLE_CENTER_Y) ? -1.f : 1.f;

        float textNearY = seatY + dirToEdge * 10.f;
        float textFarY = textNearY + dirToEdge * textBlockHeight;
        float textTopY = std::min(textNearY, textFarY);

        constexpr float CENTER_ZONE = 80.f;
        int textAnchor = 0;
        if (seatX < TABLE_CENTER_X - CENTER_ZONE) {
            textAnchor = -1;
        }
        else if (seatX > TABLE_CENTER_X + CENTER_ZONE) {
            textAnchor = 1;
        }

        float cardNearY = seatY - dirToEdge * 10.f;
        float cardFarY = cardNearY - dirToEdge * cardHeight;
        float cardTopY = std::min(cardNearY, cardFarY);

        return {cardTopY, textTopY, cardWidth, cardHeight, halfGap, textAnchor};
    }
}

int main() {
    //anti-aliasing is off by default in SFML - without it every shape edge
    //(the table oval, card corners) is drawn jagged instead of smoothed
    sf::ContextSettings settings;
    settings.antiAliasingLevel = 8;

    sf::RenderWindow window(
        sf::VideoMode({static_cast<unsigned int>(DESIGN_WIDTH), static_cast<unsigned int>(DESIGN_HEIGHT)}),
        "Poker",
        sf::Style::Default,
        sf::State::Windowed,
        settings
    );
    window.setFramerateLimit(60);

    if (!ImGui::SFML::Init(window, false)) {
        return 1;
    }

    ImGuiIO& io = ImGui::GetIO();
    //loaded first, so this becomes ImGui's default font for everything else
    io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Supplemental/Arial.ttf", 18.f);
    //two extra sizes used only by the help window's title/section headers -
    //genuinely distinct rasterized glyphs rather than a scaled-up bitmap, so
    //they stay crisp (see SetWindowFontScale's blurriness caveat elsewhere
    //in this file)
    ImFont* helpTitleFont = io.Fonts->AddFontFromFileTTF(
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf", 28.f
    );
    ImFont* helpHeaderFont = io.Fonts->AddFontFromFileTTF(
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf", 20.f
    );
    if (!ImGui::SFML::UpdateFontTexture()) {
        return 1;
    }

    sf::Texture cardSheetTexture;
    bool cardSheetLoaded = loadCardSheet(cardSheetTexture);
    if (!cardSheetLoaded) {
        std::cerr << "Warning: could not load assets/cards/deck.png from any "
                     "known working directory - card faces will not be drawn.\n";
    }
    //bilinear-filters the sheet when a sprite scales it down to its in-game
    //size instead of nearest-neighbor sampling, which looked blocky/aliased
    cardSheetTexture.setSmooth(true);

    AppMode mode = AppMode::Welcome;

    std::array<char, 16> usernameBuffer{};
    int numPlayers = 2;
    int tightCount = 1;
    int blufferCount = 0;
    int balancedCount = 0;
    int blindLevelIndex = 0;
    int buyIn = DEFAULT_BUY_IN;
    bool showHelp = false;

    std::string statusMessage;
    bool statusIsError = false;

    std::vector<std::unique_ptr<Player>> players;

    //own the running hand once "Start Game" is pressed - game/gameThread
    //outlive the Setup screen, snapshot is how the render loop (this thread)
    //and the engine thread (running game->simulate()) communicate safely
    std::unique_ptr<GameSnapshot> snapshot;
    std::unique_ptr<Game> game;
    std::thread gameThread;

    //how long a completed hand's result (showdown reveal, or just the final
    //chip counts on a fold-out win) stays on screen before the next hand deals
    constexpr std::chrono::milliseconds HAND_END_PAUSE{2500};

    //how long any move (CPU or human) stays readable before the next player acts -
    //shorter than before since a CPU's own equity calculation (CPU_SIMULATIONS in
    //player.cpp) now already takes ~300-350ms on its own, before this pause even starts
    constexpr std::chrono::milliseconds ACTION_PAUSE{1400};

    sf::Clock deltaClock;

    while (window.isOpen()) {
        while (const std::optional event = window.pollEvent()) {
            ImGui::SFML::ProcessEvent(window, *event);

            if (event->is<sf::Event::Closed>()) {
                //a mid-hand engine thread may be parked waiting on the human's
                //decision forever - detaching (rather than a graceful cancel
                //path) is enough for a desktop app that's about to exit anyway
                if (gameThread.joinable()) {
                    gameThread.detach();
                }
                window.close();
            }
        }

        ImGui::SFML::Update(window, deltaClock.restart());

        sf::Vector2u winSize = window.getSize();
        sf::Vector2f uiScale(
            static_cast<float>(winSize.x) / DESIGN_WIDTH,
            static_cast<float>(winSize.y) / DESIGN_HEIGHT
        );

        //frame-local copy of the engine thread's state - taken under the lock
        //once per frame so the rest of the frame can read it lock-free
        std::vector<PlayerView> framePlayers;
        std::vector<Card> frameBoard;
        int framePot = 0;
        int frameDealer = 0;
        int frameCurrentActorId = -1;
        std::string frameLastActionMessage;
        Action frameLastActionType = Action::Check;
        bool frameAwaitingHumanAction = false;
        std::vector<Action> frameAvailableActions;
        int frameMinRaise = 0;
        int frameMaxRaise = 0;
        bool frameGameIsOver = false;
        std::string frameWinnerName;
        bool frameWinnerIsHuman = false;
        std::array<double, 10> frameHandOdds{};
        bool frameHandOddsValid = false;

        if (mode == AppMode::Game && snapshot) {
            std::lock_guard<std::mutex> lock(snapshot->mutex);
            framePlayers = snapshot->players;
            frameBoard = snapshot->board;
            framePot = snapshot->pot;
            frameDealer = snapshot->dealer;
            frameCurrentActorId = snapshot->currentActorId;
            frameLastActionMessage = snapshot->lastActionMessage;
            frameLastActionType = snapshot->lastActionType;
            frameAwaitingHumanAction = snapshot->awaitingHumanAction;
            frameAvailableActions = snapshot->availableActions;
            frameMinRaise = snapshot->minRaise;
            frameMaxRaise = snapshot->maxRaise;
            frameGameIsOver = snapshot->gameIsOver;
            frameWinnerName = snapshot->winnerName;
            frameWinnerIsHuman = snapshot->winnerIsHuman;
            frameHandOdds = snapshot->handOdds;
            frameHandOddsValid = snapshot->handOddsValid;
        }

        std::vector<sf::Vector2f> seatPositions;
        std::vector<SeatLayout> seatLayouts;
        if (mode == AppMode::Game && !framePlayers.empty()) {
            seatPositions = computeSeatPositions(framePlayers.size());

            float textBlockHeight = ImGui::GetTextLineHeightWithSpacing() * SEAT_FONT_SCALE * 2.f;
            for (std::size_t i = 0; i < seatPositions.size(); ++i) {
                seatLayouts.push_back(computeSeatLayout(
                    seatPositions[i].x, seatPositions[i].y, i == 0, textBlockHeight
                ));
            }
        }

        ImGui::SetNextWindowPos({0.f, 0.f});
        ImGui::SetNextWindowSize({static_cast<float>(winSize.x), static_cast<float>(winSize.y)});
        //Welcome/Setup draw their widgets directly on "Poker", so it needs
        //to keep catching mouse input there - but nothing in Game mode does
        //anymore (every interactive piece - "Your Action", "Game Over",
        //"Display Hand Odds" - lives in its own floating window).
        //NoMouseInputs makes "Poker" fully click-through during
        //Game mode, so it can never again end up in front of one of those
        //and block it. This replaced an earlier approach that instead called
        //ImGui::SetNextWindowFocus() every frame on each of those floating
        //windows to force them back in front of "Poker" - that actually
        //made things worse: ImGui's FocusWindow() clears any in-progress
        //button press belonging to a different window every time it runs,
        //and a real mouse click almost always spans more than one frame
        //(press on one frame, release on a later one), so several windows
        //all re-stealing focus from each other every single frame meant
        //no click anywhere could ever survive long enough to complete
        //(see the removed comments on "Your Action" et al., which used to
        //carry that fix - each of those windows still sets its position,
        //just no longer fights for focus, since it doesn't need to anymore).
        //The same reasoning extends to showHelp: while the help window is
        //open, the three mode branches below are all skipped (see their
        //"!showHelp &&" guards), so nothing is drawn directly on "Poker"
        //there either - without NoMouseInputs here too, a stray click on the
        //empty space around the (smaller) help window would still land on
        //"Poker" and focus it to the front, where its now-empty, full-screen
        //rect would silently swallow every later click meant for the help
        //window's own buttons underneath, "Close"/"Back to Home" included -
        //indistinguishable from a freeze
        ImGuiWindowFlags pokerFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBackground;
        if (mode == AppMode::Game || showHelp) {
            pokerFlags |= ImGuiWindowFlags_NoMouseInputs;
        }
        ImGui::Begin("Poker", nullptr, pokerFlags);

        //nothing underneath renders at all while Help is open - not just
        //Welcome's Play button (as before), but every widget on whichever
        //screen was active, Setup's included - so there's no longer a
        //background screen still showing (and, in Game mode, still
        //clickable) behind/around the help window
        if (!showHelp && mode == AppMode::Welcome) {
            ImGui::SetCursorPos(toScreen({490.f, 445.f}, uiScale));
            if (ImGui::Button("Play", toScreen({300.f, 70.f}, uiScale))) {
                mode = AppMode::Setup;
            }

            ImGui::SetCursorPos(toScreen({1150.f, 30.f}, uiScale));
            if (ImGui::Button("Help", toScreen({100.f, 40.f}, uiScale))) {
                showHelp = true;
            }
        }
        else if (!showHelp && mode == AppMode::Setup) {
            ImGui::SetCursorPos(toScreen({1150.f, 30.f}, uiScale));
            if (ImGui::Button("Help", toScreen({100.f, 40.f}, uiScale))) {
                showHelp = true;
            }

            ImGui::SetCursorPos(toScreen({80.f, 80.f}, uiScale));

            ImGui::Text("Username");
            ImGui::InputText("##username", usernameBuffer.data(), usernameBuffer.size());

            ImGui::Spacing();
            ImGui::Text("Number of Players (2-10)");
            ImGui::SliderInt("##numPlayers", &numPlayers, 2, 10);

            int cpuSlots = numPlayers - 1;
            if (tightCount > cpuSlots) {
                tightCount = cpuSlots;
            }
            if (blufferCount > cpuSlots - tightCount) {
                blufferCount = cpuSlots - tightCount;
            }
            if (balancedCount > cpuSlots - tightCount - blufferCount) {
                balancedCount = cpuSlots - tightCount - blufferCount;
            }

            ImGui::Spacing();
            ImGui::Text("CPU Players (%d total)", cpuSlots);
            ImGui::SliderInt("Tight CPUs", &tightCount, 0, cpuSlots);
            if (blufferCount > cpuSlots - tightCount) {
                blufferCount = cpuSlots - tightCount;
            }
            ImGui::SliderInt("Bluffer CPUs", &blufferCount, 0, cpuSlots - tightCount);
            if (balancedCount > cpuSlots - tightCount - blufferCount) {
                balancedCount = cpuSlots - tightCount - blufferCount;
            }
            ImGui::SliderInt("Balanced CPUs", &balancedCount, 0, cpuSlots - tightCount - blufferCount);
            int advancedCount = cpuSlots - tightCount - blufferCount - balancedCount;
            ImGui::Text("Advanced CPUs: %d", advancedCount);

            //true if usernameBuffer holds a valid username; sets
            //statusIsError/statusMessage if not - the one check shared by
            //"Start Game" and "Randomize & Start Game"
            auto validateUsername = [&]() -> bool {
                std::string username(usernameBuffer.data());
                if (!isValidUsername(username)) {
                    statusIsError = true;
                    statusMessage =
                        "Username must be 3-15 characters, start with a "
                        "letter, and contain no whitespace.";
                    return false;
                }
                statusIsError = false;
                return true;
            };

            //builds real engine players from the current tightCount/
            //blufferCount/balancedCount/advancedCount split and starts the
            //hand - assumes the caller already confirmed validateUsername().
            //Recomputes cpuSlots/advancedCount itself instead of closing
            //over the values already computed above this frame, since
            //"Randomize & Start Game" changes tightCount/blufferCount/
            //balancedCount immediately before calling this
            auto startGame = [&]() {
                std::string username(usernameBuffer.data());
                const BlindLevel& blinds = blindLevels[blindLevelIndex];
                Player::bigBlind = blinds.bigBlind;

                std::shuffle(cpuNames.begin(), cpuNames.end(), uiRng);

                snapshot = std::make_unique<GameSnapshot>();

                players.clear();
                players.push_back(std::make_unique<GuiHuman>(username, buyIn, 0, snapshot.get()));

                int startCpuSlots = numPlayers - 1;
                int startAdvancedCount = startCpuSlots - tightCount - blufferCount - balancedCount;

                int id = 1;
                int nameIndex = 0;
                for (int i = 0; i < tightCount; ++i) {
                    players.push_back(createCPUPlayer(
                        PlayerType::TightCPU,
                        cpuNames[nameIndex++],
                        buyIn,
                        id++
                    ));
                }
                for (int i = 0; i < blufferCount; ++i) {
                    players.push_back(createCPUPlayer(
                        PlayerType::BlufferCPU,
                        cpuNames[nameIndex++],
                        buyIn,
                        id++
                    ));
                }
                for (int i = 0; i < balancedCount; ++i) {
                    players.push_back(createCPUPlayer(
                        PlayerType::BalancedCPU,
                        cpuNames[nameIndex++],
                        buyIn,
                        id++
                    ));
                }
                for (int i = 0; i < startAdvancedCount; ++i) {
                    players.push_back(createCPUPlayer(
                        PlayerType::AdvancedCPU,
                        cpuNames[nameIndex++],
                        buyIn,
                        id++
                    ));
                }

                if (gameThread.joinable()) {
                    gameThread.detach();
                }

                game = std::make_unique<Game>(
                    std::move(players), buyIn, blinds.smallBlind, blinds.bigBlind
                );

                GameSnapshot* snapshotPtr = snapshot.get();
                game->onHandStart = [snapshotPtr](const Game& g) {
                    refreshSnapshot(*snapshotPtr, g);
                    //new hand, empty board - this is the one guaranteed
                    //point to (re)compute preflop odds for the fresh hole cards
                    refreshHandOdds(*snapshotPtr, g);
                    std::lock_guard<std::mutex> lock(snapshotPtr->mutex);
                    snapshotPtr->lastActionMessage.clear();
                };
                game->onTurnStart = [snapshotPtr](const Game& g, const Player& p) {
                    //refreshes board/pot/players first - without this, a
                    //player who is first to act on a brand new street (no
                    //intervening onAction to pick up dealTurn()/dealRiver()'s
                    //new card) would see the previous street's stale board
                    refreshSnapshot(*snapshotPtr, g);
                    refreshHandOdds(*snapshotPtr, g);
                    std::lock_guard<std::mutex> lock(snapshotPtr->mutex);
                    snapshotPtr->currentActorId = p.getID();
                    //clears the previous actor's message so it doesn't
                    //linger, mismatched, once the dot moves to this seat
                    snapshotPtr->lastActionMessage.clear();
                };
                game->onAction = [snapshotPtr, ACTION_PAUSE](
                    const Game& g, const Player& p, const PlayerAction& a, bool reopenedBetting
                ) {
                    refreshSnapshot(*snapshotPtr, g);
                    //also caught here (not just onTurnStart/onHandStart) so
                    //the odds panel updates even on a street nobody acts on,
                    //e.g. an all-in runout with cards still to come
                    refreshHandOdds(*snapshotPtr, g);
                    {
                        std::lock_guard<std::mutex> lock(snapshotPtr->mutex);
                        snapshotPtr->lastActionMessage = describeAction(p, a, reopenedBetting);
                        snapshotPtr->lastActionType = a.action;
                    }

                    std::this_thread::sleep_for(ACTION_PAUSE);
                };
                game->onHandEnd = [snapshotPtr, HAND_END_PAUSE](const Game& g, bool showdown) {
                    refreshSnapshot(*snapshotPtr, g, showdown);
                    {
                        std::lock_guard<std::mutex> lock(snapshotPtr->mutex);
                        snapshotPtr->currentActorId = -1;
                    }

                    std::this_thread::sleep_for(HAND_END_PAUSE);
                };
                game->onGameOver = [snapshotPtr](const Game&, const Player& winner) {
                    std::lock_guard<std::mutex> lock(snapshotPtr->mutex);
                    snapshotPtr->gameIsOver = true;
                    snapshotPtr->winnerName = winner.getName();
                    //human is always constructed with id 0, see the GuiHuman
                    //push_back above
                    snapshotPtr->winnerIsHuman = winner.getID() == 0;
                };

                gameThread = std::thread(&Game::simulate, game.get());

                mode = AppMode::Game;
            };

            //randomizes the CPU split and starts the hand in one click,
            //without ever displaying the chosen split first - validates the
            //username same as "Start Game" (and shows the same error if
            //invalid), but leaves tightCount/blufferCount/balancedCount
            //untouched in that case rather than randomizing into a state the
            //user can't see and didn't get to start
            if (ImGui::Button("Randomize & Start Game")) {
                if (validateUsername()) {
                    std::uniform_int_distribution<int> tightDist(0, cpuSlots);
                    tightCount = tightDist(uiRng);
                    std::uniform_int_distribution<int> blufferDist(0, cpuSlots - tightCount);
                    blufferCount = blufferDist(uiRng);
                    std::uniform_int_distribution<int> balancedDist(0, cpuSlots - tightCount - blufferCount);
                    balancedCount = balancedDist(uiRng);

                    startGame();
                }
            }

            ImGui::Spacing();
            ImGui::Text("Starting Chips (%d-%d)", MIN_BUY_IN, MAX_BUY_IN);
            ImGui::SliderInt("##buyIn", &buyIn, MIN_BUY_IN, MAX_BUY_IN);

            ImGui::Spacing();
            ImGui::Text("Blinds");
            for (int i = 0; i < static_cast<int>(blindLevels.size()); ++i) {
                if (ImGui::RadioButton(blindLevels[i].label, blindLevelIndex == i)) {
                    blindLevelIndex = i;
                }
                if (i + 1 < static_cast<int>(blindLevels.size())) {
                    ImGui::SameLine();
                }
            }

            ImGui::Spacing();
            if (ImGui::Button("Start Game", toScreen({200.f, 50.f}, uiScale))) {
                if (validateUsername()) {
                    startGame();
                }
            }

            if (!statusMessage.empty()) {
                ImGui::Spacing();
                if (statusIsError) {
                    ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", statusMessage.c_str());
                }
                else {
                    ImGui::TextColored({0.4f, 1.f, 0.6f, 1.f}, "%s", statusMessage.c_str());
                }
            }
        }
        else if (!showHelp && mode == AppMode::Game) {
            if (framePlayers.empty()) {
                ImGui::SetCursorPos(toScreen({560.f, 460.f}, uiScale));
                ImGui::Text("Shuffling up and dealing...");
            }
            else {
                ImGui::SetWindowFontScale(SEAT_FONT_SCALE);

                for (std::size_t i = 0; i < framePlayers.size(); ++i) {
                    const PlayerView& player = framePlayers[i];
                    sf::Vector2f pos = seatPositions[i];
                    const SeatLayout& layout = seatLayouts[i];

                    std::string nameLine = player.name
                        + seatPositionLabel(i, frameDealer, framePlayers.size());
                    std::string stackLine = formatStack(player.chips);

                    //the current actor's most recent action tags onto the end
                    //of their stack line (" Raise", colored) rather than a
                    //separate third line, so the seat's text block never
                    //grows taller than the 2 lines the layout already assumes
                    bool showAction = player.id == frameCurrentActorId && !frameLastActionMessage.empty();
                    std::string actionSuffix = showAction ? " " + frameLastActionMessage : "";

                    //size each label on its own rendered width instead of a fixed
                    //guess, since names/stacks/actions vary in length per player -
                    //CalcTextSize doesn't account for SetWindowFontScale, so its
                    //result is multiplied by the same scale manually
                    float textWidth = std::max(
                        ImGui::CalcTextSize(nameLine.c_str()).x,
                        ImGui::CalcTextSize((stackLine + actionSuffix).c_str()).x
                    ) * SEAT_FONT_SCALE;

                    //left/right seats grow their text away from the table's
                    //horizontal center instead of centering it, so neighboring
                    //seats on the same side don't grow toward each other
                    float xOffset = layout.textAnchor == 0 ? -textWidth / 2.f
                                  : layout.textAnchor < 0 ? -textWidth
                                                          : 0.f;

                    ImVec2 screenPos = toScreen({pos.x, layout.textTopY}, uiScale);
                    ImGui::SetCursorPos({screenPos.x + xOffset, screenPos.y});
                    ImGui::BeginGroup();
                    ImGui::Text("%s", nameLine.c_str());
                    ImGui::Text("%s", stackLine.c_str());
                    if (showAction) {
                        ImGui::SameLine();
                        ImGui::TextColored(actionColor(frameLastActionType), "%s", actionSuffix.c_str());
                    }
                    ImGui::EndGroup();
                }

                ImGui::SetWindowFontScale(1.0f);

                ImGui::SetCursorPos(toScreen({TABLE_CENTER_X - 60.f, TABLE_CENTER_Y + 80.f}, uiScale));
                ImGui::Text("Pot: %s", formatChips(framePot).c_str());

                //off by default - the panel below only shows once the user
                //asks for it, by clicking this any time mid-hand, not
                //automatically on every hand. Lives in its own floating
                //window (rather than directly in the borderless full-screen
                //"Poker" window, like the pot text above) - no
                //SetNextWindowFocus() needed since "Poker" already can't
                //steal focus during Game mode (see its NoMouseInputs flag
                //above). An earlier version of this called
                //SetNextWindowFocus() here every frame as an extra safety
                //measure, but that's actively harmful: ImGui's FocusWindow()
                //clears any in-progress button press belonging to a
                //different window every time it's called, and a real mouse
                //click almost always spans more than one frame (press on
                //one frame, release on a later one) - so unconditionally
                //stealing focus here every single frame made it impossible
                //for a click anywhere in "Your Action" (drawn after this)
                //to ever complete, and vice versa for this button during
                //the human's own turn
                static bool showHandOdds = false;
                ImGui::SetNextWindowPos(toScreen({TABLE_CENTER_X - 90.f, TABLE_CENTER_Y + 110.f}, uiScale));
                ImGui::Begin(
                    "Hand Odds Toggle", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize
                );
                if (ImGui::Button("Display Hand Odds")) {
                    showHandOdds = !showHandOdds;
                }
                ImGui::End();

                //same floating-toggle pattern as "Hand Odds Toggle" above,
                //tucked into the opposite (top-left) corner so it stays clear
                //of the seat ellipse and the pot text
                ImGui::SetNextWindowPos(toScreen({20.f, 5.f}, uiScale));
                ImGui::Begin(
                    "Help Toggle", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize
                );
                if (ImGui::Button("Help")) {
                    showHelp = true;
                }
                ImGui::End();

                //tucked into a thin strip along the very top edge, clear of
                //the seat ellipse (the closest seat labels, upper-right at
                //design y~254, grow their text up to roughly y~200 - this
                //panel stays above that with margin to spare) - split into
                //two compact columns instead of one tall list so it fits in
                //that strip. Only shown once the user opts in via the
                //button above, and only while the human has a live hand to
                //show odds for (hidden once they've folded, or before the
                //first hand's odds are computed)
                if (showHandOdds && frameHandOddsValid) {
                    ImGui::SetNextWindowPos(toScreen({940.f, 5.f}, uiScale));
                    //no SetNextWindowSize - sizing to fit its own content
                    //avoids a block of empty space below a short list, and
                    //keeps the font at native scale (SetWindowFontScale
                    //below 1.0 shrinks the already-rasterized glyph bitmaps
                    //rather than re-rendering them sharp at the new size,
                    //which is why the panel looked blurry with it)
                    ImGui::Begin(
                        "Your Odds", nullptr,
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize
                    );

                    //left column: high card through straight; right column:
                    //flush through royal flush - an even split down the middle
                    constexpr int ODDS_COLUMN_SPLIT = 5;

                    ImGui::BeginGroup();
                    for (int category = 0; category < ODDS_COLUMN_SPLIT; ++category) {
                        //skips categories that are flat-out impossible with
                        //this hand (e.g. a royal flush holding a deuce),
                        //rather than cluttering the list with 0.0% rows
                        if (frameHandOdds[category] > 0.0) {
                            ImGui::Text(
                                "%s: %.1f%%",
                                HAND_CATEGORY_NAMES[category],
                                frameHandOdds[category]
                            );
                        }
                    }
                    ImGui::EndGroup();

                    ImGui::SameLine(170.f);

                    ImGui::BeginGroup();
                    for (int category = ODDS_COLUMN_SPLIT;
                         category < static_cast<int>(frameHandOdds.size()); ++category) {
                        if (frameHandOdds[category] > 0.0) {
                            ImGui::Text(
                                "%s: %.1f%%",
                                HAND_CATEGORY_NAMES[category],
                                frameHandOdds[category]
                            );
                        }
                    }
                    ImGui::EndGroup();

                    ImGui::End();
                }
            }

            //only the human ever blocks waiting on an action - every other
            //seat is a CPU that decides and acts without GUI involvement
            if (frameAwaitingHumanAction && snapshot) {
                int callAmount = 0;
                if (!framePlayers.empty()) {
                    int highestBet = 0;
                    for (const PlayerView& p : framePlayers) {
                        highestBet = std::max(highestBet, p.currentBet);
                    }
                    callAmount = highestBet - framePlayers[0].currentBet;
                }

                static int raiseAmount = frameMinRaise;
                raiseAmount = std::clamp(raiseAmount, frameMinRaise, std::max(frameMinRaise, frameMaxRaise));

                //sits below the human's own seat label (name + chip count,
                //rendered around y 770-830 for the bottom seat) rather than
                //on top of it - the panel used to be positioned at y 760 and
                //hid that text every time it was the human's turn to act
                ImGui::SetNextWindowPos(toScreen({390.f, 840.f}, uiScale));
                ImGui::SetNextWindowSize(toScreen({500.f, 120.f}, uiScale));
                //no SetNextWindowFocus() here - see the comment on "Hand Odds
                //Toggle" above for why. The invisible full-screen "Poker"
                //window (see ImGui::Begin("Poker", ...) below) already can't
                //steal focus during Game mode (its NoMouseInputs flag), so
                //this window doesn't need to keep re-asserting focus over it
                //every frame; doing so anyway used to actively break clicks
                //on these very buttons, by clearing their in-progress press
                //state out from under them before the release could land
                ImGui::Begin(
                    "Your Action", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
                );

                auto sendDecision = [&](Action action, int amount) {
                    std::lock_guard<std::mutex> lock(snapshot->mutex);
                    snapshot->decision = {action, amount};
                    snapshot->decisionReady = true;
                    snapshot->cv.notify_one();
                };

                for (Action action : frameAvailableActions) {
                    if (action == Action::Raise) {
                        continue;
                    }

                    std::string label = actionToString(action);
                    if (action == Action::Call) {
                        label += " (" + formatChips(callAmount) + ")";
                    }

                    if (ImGui::Button(label.c_str())) {
                        sendDecision(action, 0);
                    }
                    ImGui::SameLine();
                }

                bool canRaise = std::find(
                    frameAvailableActions.begin(), frameAvailableActions.end(), Action::Raise
                ) != frameAvailableActions.end();

                if (canRaise) {
                    ImGui::NewLine();
                    ImGui::SetNextItemWidth(300.f);
                    ImGui::SliderInt("Raise amount", &raiseAmount, frameMinRaise, frameMaxRaise);
                    ImGui::SameLine();
                    if (ImGui::Button("Raise")) {
                        sendDecision(Action::Raise, raiseAmount);
                    }
                }

                ImGui::End();
            }

            //fires once, from the onGameOver hook, when only one player has
            //chips left - everyone else has been eliminated over however
            //many hands were played. The table underneath stays frozen at
            //its last state (nothing more will refresh it, since the engine
            //thread's simulate() loop has already exited or is about to)
            if (frameGameIsOver) {
                ImGui::SetNextWindowPos(toScreen({440.f, 400.f}, uiScale));
                ImGui::SetNextWindowSize(toScreen({400.f, 180.f}, uiScale));
                //no SetNextWindowFocus() here - see the comment on "Your
                //Action" above
                ImGui::Begin(
                    "Game Over", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
                );

                if (frameWinnerIsHuman) {
                    ImGui::Text("You won the game!");
                }
                else {
                    ImGui::Text("%s won the game!", frameWinnerName.c_str());
                }

                ImGui::Spacing();
                if (ImGui::Button("Return to Home", toScreen({200.f, 50.f}, uiScale))) {
                    //simulate()'s loop has already exited (or is about to,
                    //with nothing left to block on) by the time onGameOver
                    //fires, so this returns essentially immediately
                    if (gameThread.joinable()) {
                        gameThread.join();
                    }
                    mode = AppMode::Welcome;
                }

                ImGui::End();
            }
        }

        //draws the reference doc explaining CPU personalities, Setup screen
        //options, and the Hand Odds panel. Pinned in place (NoMove/NoResize)
        //rather than freely draggable like a normal ImGui window - letting it
        //move or resize risks it ending up on top of "Your Action" during
        //Game mode (the two don't overlap at this fixed rect, but they can
        //once this window's dragged/enlarged), and since "Your Action" is the
        //only thing GuiHuman::act() is waiting on to unblock, covering it
        //leaves the game sitting there indefinitely - indistinguishable from
        //a genuine hang. Position/size are set unconditionally (no
        //ImGuiCond_FirstUseEver) every frame rather than once, so a stale
        //position an older build of this window may have already written to
        //imgui.ini can't override this fixed layout either. Defined here
        //(immediately before its one call site, rather than up near uiScale)
        //so it can read this frame's already-computed frameGameIsOver - its
        //own independent window, so drawing it after the mode branch above
        //(rather than inside one particular mode) is what lets it stay open
        //across a mode switch, e.g. opened from Welcome and still open once
        //"Play" moves on to Setup
        auto drawHelpWindow = [&]() {
            if (!showHelp) {
                return;
            }

            ImGui::SetNextWindowSize(toScreen({620.f, 700.f}, uiScale));
            ImGui::SetNextWindowPos(toScreen({330.f, 130.f}, uiScale));
            //no p_open/title-bar close button here - the "Back to Home"/
            //"Close" button drawn as the first thing inside is the one and
            //only way to dismiss this window, so there's exactly one place
            //to look for it. A title-bar close button living wherever the
            //user last dragged the window works fine on its own, but paired
            //with a second, differently-scoped control also claiming to
            //close/navigate, the two could disagree (e.g. title-bar close
            //only closing the window while leaving mode on Setup, right
            //after the button below already navigated home) - one control
            //avoids that entirely
            ImGui::Begin(
                "Poker Help", nullptr,
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse
            );

            auto sectionHeader = [&](const char* text) {
                ImGui::Spacing();
                ImGui::PushFont(helpHeaderFont);
                ImGui::TextColored({0.55f, 0.85f, 0.55f, 1.f}, "%s", text);
                ImGui::PopFont();
                ImGui::Separator();
                ImGui::Spacing();
            };

            //a bulleted, colored name followed by an indented wrapped
            //description on its own line - avoids the misaligned wrapping
            //ImGui::TextWrapped produces when placed via SameLine() right
            //after a bullet
            auto entry = [&](const char* name, ImVec4 color, const char* desc) {
                ImGui::Bullet();
                ImGui::SameLine();
                ImGui::TextColored(color, "%s", name);
                ImGui::Indent();
                ImGui::TextWrapped("%s", desc);
                ImGui::Unindent();
                ImGui::Spacing();
            };

            ImGui::PushFont(helpTitleFont);
            ImGui::TextColored({0.95f, 0.95f, 0.95f, 1.f}, "Poker Help");
            ImGui::PopFont();

            //always reachable regardless of where this window's been moved
            //or resized to, unlike a same-purpose button living at a fixed
            //spot in one of the mode screens underneath, which this window
            //could easily end up covering once dragged around - drawn as the
            //very first thing here so it's visible before any scrolling.
            //Safe to navigate all the way back to the Welcome/home screen
            //from anywhere except mid-hand in Game mode, where a live engine
            //thread is still running a hand nobody's asked to abandon; there
            //it just closes this window instead, leaving the hand running
            //exactly as it was
            bool safeToGoHome = mode != AppMode::Game || frameGameIsOver;
            if (ImGui::Button(safeToGoHome ? "Back to Home" : "Close")) {
                showHelp = false;
                if (safeToGoHome && mode != AppMode::Welcome) {
                    //mirrors the Game Over screen's own "Return to Home"
                    //button exactly - by the time frameGameIsOver is true,
                    //simulate()'s loop has already exited (or is one no-op
                    //iteration from it), so this join returns essentially
                    //immediately rather than actually blocking
                    if (gameThread.joinable()) {
                        gameThread.join();
                    }
                    mode = AppMode::Welcome;
                }
            }
            ImGui::Separator();

            sectionHeader("Game Setup");

            ImGui::TextColored({0.85f, 0.85f, 0.9f, 1.f}, "Username");
            ImGui::TextWrapped(
                "Enter a name with 3 to 15 characters. It must begin with a letter and cannot "
                "contain spaces."
            );
            ImGui::Spacing();

            ImGui::TextColored({0.85f, 0.85f, 0.9f, 1.f}, "CPU Players");
            ImGui::TextWrapped(
                "Every CPU is given a random, meaningless display name (e.g. \"ChipBot7\") "
                "regardless of its type, and it never announces which of the four "
                "personalities below it's playing."
            );
            ImGui::Spacing();

            entry(
                "Tight CPU", {0.55f, 0.70f, 0.95f, 1.f},
                "Only plays a narrow range of strong starting hands, and needs a bigger edge "
                "over the pot odds before calling or raising, so it usually enters and stays "
                "in fewer pots than the others."
            );
            entry(
                "Bluffer CPU", {0.95f, 0.65f, 0.35f, 1.f},
                "Plays a much wider range of hands and bluffs far more often, though how often "
                "still depends on the situation. It bluffs less into a crowded pot or against "
                "a deep, heavily stacked opponent, and cools off for a few hands after a bluff "
                "gets called."
            );
            entry(
                "Balanced CPU", {0.45f, 0.85f, 0.75f, 1.f},
                "Tracks how every opponent at the table actually plays, including how often "
                "they enter pots, fold, bet, and bluff, and leans its own calls, raises, and "
                "bluffs against that read once it's seen enough hands from them. It also "
                "adjusts its own tendencies over time based on whether its recent hands won, "
                "lost, or were folded."
            );
            entry(
                "Advanced CPU", {0.75f, 0.55f, 0.95f, 1.f},
                "Remembers every action any player takes, in every hand, not just the ones "
                "that reach showdown, so it can learn from how you played even when your "
                "cards were never shown. It also leans on real game theory math (bet sizing "
                "tells, minimum defense frequency, mixed strategies) rather than a single "
                "predictable cutoff. It never looks at your actual hole cards while deciding "
                "what to do in the hand you're currently playing, only at actions that have "
                "already resolved."
            );

            ImGui::TextWrapped(
                "These are tendencies, not fixed rules, since a Tight CPU can still bluff "
                "occasionally, and a Bluffer CPU can still fold a bad hand."
            );
            ImGui::Spacing();

            ImGui::TextColored({0.85f, 0.85f, 0.9f, 1.f}, "Starting Chips & Blinds");
            ImGui::TextWrapped(
                "Choose everyone's starting stack (100 to 1,000 chips) and a blind level "
                "(1/2, 2/5, or 5/10) before starting. You and every CPU begin with the same "
                "stack."
            );
            ImGui::Spacing();

            ImGui::TextColored({0.85f, 0.85f, 0.9f, 1.f}, "Start Game / Randomize & Start Game");
            ImGui::TextWrapped(
                "Start Game deals you in with the exact CPU split you chose above. Randomize "
                "& Start Game instead picks a random mix of CPU types for you and starts right "
                "away, without ever showing you what it picked."
            );

            sectionHeader("Playing a Hand");
            ImGui::TextWrapped(
                "On your turn, an action panel appears below your seat with whichever of Fold, "
                "Check, Call, Raise, and All In are legal right now. Raising opens a slider "
                "between the minimum and maximum legal amount before you confirm. A red dot "
                "marks whoever's currently deciding, and each seat briefly shows its most "
                "recent action next to its chip count."
            );

            sectionHeader("Hand Odds");
            ImGui::TextWrapped(
                "Click \"Display Hand Odds\" during a hand to see the exact percentage chance "
                "your final hand ends up in each poker hand category, based on your hole cards "
                "and however much of the board is revealed so far. These are computed exactly, "
                "by checking every possible way the remaining cards could come out, not "
                "estimated, and update automatically as new cards are dealt. Categories that "
                "are impossible with your cards (0%%) are left off the list."
            );
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Percentages are rounded to one decimal place for display, so an extremely "
                "small but nonzero chance (say, 0.02%%) can still show up as 0.0%%."
            );

            ImGui::End();
        };
        drawHelpWindow();

        ImGui::End();

        window.clear(sf::Color(20, 55, 35));

        if (mode == AppMode::Game) {
            sf::ConvexShape tableBorder = makeStadium(1140.f, 790.f);
            tableBorder.setPosition({70.f, 85.f});
            tableBorder.setFillColor(sf::Color(45, 45, 45));
            window.draw(tableBorder);

            sf::ConvexShape tableFelt = makeStadium(1100.f, 750.f);
            tableFelt.setPosition({90.f, 105.f});
            tableFelt.setFillColor(sf::Color(115, 185, 120));
            window.draw(tableFelt);

            //thin inset rail line, not a filled shape - matches the reference table
            sf::ConvexShape innerRail = makeStadium(1000.f, 650.f);
            innerRail.setPosition({140.f, 155.f});
            innerRail.setFillColor(sf::Color::Transparent);
            innerRail.setOutlineColor(sf::Color(110, 175, 110));
            innerRail.setOutlineThickness(3.f);
            window.draw(innerRail);

            //faint card-slot markers for where the board will render later
            constexpr float SLOT_WIDTH = 105.f;
            constexpr float SLOT_HEIGHT = 134.f;
            constexpr float SLOT_GAP = 12.f;
            constexpr int NUM_SLOTS = 5;
            float slotsWidth = NUM_SLOTS * SLOT_WIDTH + (NUM_SLOTS - 1) * SLOT_GAP;
            float slotStartX = TABLE_CENTER_X - slotsWidth / 2.f;
            float slotY = TABLE_CENTER_Y - SLOT_HEIGHT / 2.f;

            for (int i = 0; i < NUM_SLOTS; ++i) {
                sf::RectangleShape slot({SLOT_WIDTH, SLOT_HEIGHT});
                slot.setPosition({slotStartX + i * (SLOT_WIDTH + SLOT_GAP), slotY});
                slot.setFillColor(sf::Color::Transparent);
                slot.setOutlineColor(sf::Color(255, 255, 255, 60));
                slot.setOutlineThickness(2.f);
                window.draw(slot);

                if (static_cast<std::size_t>(i) < frameBoard.size()) {
                    drawCardFace(
                        window, cardSheetTexture, cardSheetLoaded, frameBoard[static_cast<std::size_t>(i)],
                        {slotStartX + i * (SLOT_WIDTH + SLOT_GAP), slotY}, {SLOT_WIDTH, SLOT_HEIGHT}
                    );
                }
            }

            //each seat's hole cards - players[0] is always the human (see the
            //Start Game handler above) and always sees their own cards face-up;
            //other seats stay face-down until framePlayers[i].handRevealed is
            //set at a genuine showdown (see the onHandEnd hook)
            for (std::size_t i = 0; i < seatPositions.size(); ++i) {
                sf::Vector2f pos = seatPositions[i];
                const SeatLayout& layout = seatLayouts[i];
                const PlayerView& player = framePlayers[i];

                sf::Vector2f card1Pos{pos.x - layout.halfGap - layout.cardWidth, layout.cardTopY};
                sf::Vector2f card2Pos{pos.x + layout.halfGap, layout.cardTopY};
                sf::Vector2f cardSize{layout.cardWidth, layout.cardHeight};

                //pos itself sits in the small gap between the cards (toward
                //the table center) and the name/chip text (toward the rail),
                //so a dot centered there sits cleanly between them for
                //whichever seat is currently on the clock
                if (player.id == frameCurrentActorId) {
                    sf::CircleShape turnDot(8.f);
                    turnDot.setOrigin({8.f, 8.f});
                    turnDot.setPosition(pos);
                    turnDot.setFillColor(sf::Color::Red);
                    turnDot.setOutlineColor(sf::Color::White);
                    turnDot.setOutlineThickness(1.f);
                    window.draw(turnDot);
                }

                //a folded player's cards are mucked - nothing left to show
                if (!player.folded) {
                    if (player.handRevealed) {
                        drawCardFace(window, cardSheetTexture, cardSheetLoaded, player.hand[0], card1Pos, cardSize);
                        drawCardFace(window, cardSheetTexture, cardSheetLoaded, player.hand[1], card2Pos, cardSize);
                    }
                    else {
                        sf::RectangleShape cardBack1(cardSize);
                        cardBack1.setPosition(card1Pos);
                        cardBack1.setFillColor(sf::Color(40, 40, 90));
                        cardBack1.setOutlineColor(sf::Color::White);
                        cardBack1.setOutlineThickness(1.f);
                        window.draw(cardBack1);

                        sf::RectangleShape cardBack2(cardSize);
                        cardBack2.setPosition(card2Pos);
                        cardBack2.setFillColor(sf::Color(40, 40, 90));
                        cardBack2.setOutlineColor(sf::Color::White);
                        cardBack2.setOutlineThickness(1.f);
                        window.draw(cardBack2);
                    }
                }
            }
        }

        ImGui::SFML::Render(window);
        window.display();
    }

    ImGui::SFML::Shutdown();
    return 0;
}
