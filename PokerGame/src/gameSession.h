#pragma once

#include <array>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "card.h"
#include "game.h"
#include "handEval.h"
#include "player.h"

//EFFECTS: one seat's plain-data snapshot, safe to read from the render thread
struct PlayerView{
    int id = -1;
    std::string name;
    int chips = 0;
    int currentBet = 0;
    bool folded = false;
    bool allIn = false;
    bool isHuman = false;
    std::array<Card, 2> hand{};
    bool handRevealed = false;
};

//EFFECTS: mutex-guarded state shared between the engine thread (running
//         Game::simulate()) and the render thread - the engine thread is the
//         only writer of game state and the only reader of the human-decision
//         fields; the render thread is the only writer of the human-decision
//         fields and the only reader of everything else
struct GameSnapshot{
    std::mutex mutex;
    std::condition_variable cv;

    std::vector<PlayerView> players;
    std::vector<Card> board;
    int pot = 0;
    int dealer = 0;

    //set once by the onGameOver hook when only one player has chips left -
    //read by the render thread to show a game-over screen instead of the
    //normal table. By the time this is set, awaitingHumanAction is already
    //false (onGameOver fires after onHandEnd has already returned), so
    //there's no overlap with that panel
    bool gameIsOver = false;
    std::string winnerName;
    bool winnerIsHuman = false;

    //id of the player whose turn it currently is (matches PlayerView::id),
    //or -1 when no one is currently on the clock (between hands); set by the
    //onTurnStart hook, used by the render thread to draw a turn indicator
    int currentActorId = -1;

    //short label for currentActorId's most recent action (e.g. "Raise"),
    //set by the onAction hook and cleared by onTurnStart once the next
    //player's turn begins; lastActionType selects the label's display color
    std::string lastActionMessage;
    Action lastActionType = Action::Check;

    //set by GuiHuman::act() when it's the human's turn, read by the render
    //thread to decide whether to show the action panel
    bool awaitingHumanAction = false;
    std::vector<Action> availableActions;
    int minRaise = 0;
    int maxRaise = 0;

    //set by the render thread on a button click, read by GuiHuman::act()
    bool decisionReady = false;
    PlayerAction decision{};

    //the human's exact odds (in %) of ending up with each hand category,
    //indexed by HandCategory - set by refreshHandOdds(), read by the render
    //thread to draw the odds panel. Only meaningful while handOddsValid is set
    std::array<double, 10> handOdds{};
    bool handOddsValid = false;

    //the board size handOdds was last computed for - lets refreshHandOdds()
    //tell whether a recompute is actually needed instead of redoing
    //Calculator's brute force (expensive on an empty preflop board) on
    //every single call
    int handOddsBoardSize = -1;
};

//MODIFIES: snapshot
//EFFECTS: locks snapshot's mutex and rebuilds players/board/pot/dealer from
//         the game's current state. Each player's handRevealed resets to
//         whether they're the human, which is what clears a previous hand's
//         showdown reveal once the next hand starts. When revealAll is true
//         (a genuine showdown was reached), every non-folded player's hand
//         gets revealed too
void refreshSnapshot(GameSnapshot &snapshot, const Game &game, bool revealAll = false);

//MODIFIES: snapshot
//EFFECTS: recomputes the human's exact odds of ending up with each hand
//         category (via Calculator) if the board has grown since the last
//         computation - a no-op otherwise, since redoing Calculator's brute
//         force on an empty preflop board is expensive, and there's no
//         reason to repeat it when nothing's changed. Clears handOddsValid
//         when there's no human seated, or they've folded out of the hand
void refreshHandOdds(GameSnapshot &snapshot, const Game &game);

//EFFECTS: a short label for the given action (e.g. "Raise to 80"), used as
//         the colored per-seat action announcement. Raise always includes
//         the player's post-action total bet; AllIn only includes it when
//         reopenedBetting is true (a shove that doesn't reopen betting is
//         effectively a capped call, so no amount is shown for it). Call
//         this after the action's already been applied, so player's
//         currentBet reflects it
std::string describeAction(const Player &player, const PlayerAction &action, bool reopenedBetting);

//a human player whose act() hands the decision off to the render thread
//through a GameSnapshot instead of blocking on std::cin/cout, so it can run
//on a background thread underneath an SFML/ImGui loop
class GuiHuman: public Player{
    public:
        //EFFECTS: sets up the underlying player and stores the snapshot
        //         used to talk to the render thread
        GuiHuman(const std::string &nameIn, int chipsIn, int playerIDin, GameSnapshot *snapshotIn);

        //EFFECTS: publishes the available actions/raise bounds and blocks
        //         until the render thread hands back a decision
        PlayerAction act(const BasicCPUState& basicState) override;

    private:
        GameSnapshot *snapshot;
};
