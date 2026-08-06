#pragma once

#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "card.h"
#include "player.h"

enum class Action {
    Fold,
    Check,
    Call,
    Raise,
    AllIn
};

//EFFECTS: turns an Action into its display name ("Raise", "Fold", etc.)
std::string actionToString(Action action);

class Game{
    private:
        std::vector<std::unique_ptr<Player>> players;
        std::vector<Card> deck;
        std::vector<Card> board;
        std::mt19937 rng{std::random_device{}()};
        Player* lastAggressor = nullptr;
        int dealer = 0;
        int numPlayers;
        int playersInHand;
        int smallBlindCost;
        int bigBlindCost;
        int totalHands = 0;
        int pot = 0;
        int currentHighestBet;
        int lastRaiseSize;
        int callersThisStreet = 0;
        int nextCard;
        bool gameOver = false;

        //MODIFIES: deck
        //EFFECTS: shuffles the deck
        void shuffle();

    public:
        //fires in simulate() right after dealPlayers() - lets a caller (e.g. a
        //GUI) refresh its view as soon as a new hand's cards exist
        std::function<void(const Game&)> onHandStart;

        //fires in round() right before a live player acts - lets a caller
        //(e.g. a GUI) show whose turn it is before their decision is known
        std::function<void(const Game&, const Player&)> onTurnStart;

        //fires at the end of applyAction() - lets a caller refresh state and
        //report what the player just did; the bool is true only when the
        //action reopened betting for everyone else (always true for a full
        //raise, only sometimes for an all-in - see applyAction's AllIn case)
        std::function<void(const Game&, const Player&, const PlayerAction&, bool)> onAction;

        //fires once the pot's paid out, but before reset() clears hands/board -
        //lets a caller grab final hole cards/chip counts for a showdown
        //display; the bool is true only for a genuine showdown, not a fold-out win
        std::function<void(const Game&, bool)> onHandEnd;

        //fires once, from reset(), when eliminate() has left only one player
        //with chips and the game is over - passes that winner. Fires after
        //onHandEnd for the final hand; simulate()'s loop exits right after,
        //so a caller shouldn't expect any further hooks once this fires
        std::function<void(const Game&, const Player&)> onGameOver;

        //MODIFIES: players, buyIn, deck, numplayers, smallBlindCost, bigBlindCost
        //EFFECTS: sets up the players, their chip counts, and a fresh deck
        Game(std::vector<std::unique_ptr<Player>> playersIn, int buyIn,
                                   int smallBlindIn, int bigBlindIn);

        //MODIFIES: game state
        //EFFECTS: plays hands one after another until the game is over
        void simulate();

        //EFFECTS: the players in the game
        const std::vector<std::unique_ptr<Player>>& getPlayers() const;

        //EFFECTS: the current board
        const std::vector<Card>& getBoard() const;

        //EFFECTS: the current pot size
        int getPot() const;

        //EFFECTS: the current dealer's seat index
        int getDealer() const;

        //MODIFIES: each remaining player's hand, nextCard
        //EFFECTS: deals two cards to each remaining player
        void dealPlayers();

        //MODIFIES: board, nextCard
        //EFFECTS: burns a card and deals the flop
        void dealFlop();

        //MODIFIES: board, nextCard
        //EFFECTS: burns a card and deals the turn
        void dealTurn();

        //MODIFIES: board, nextCard
        //EFFECTS: burns a card and deals the river
        void dealRiver();

        //MODIFIES: nextCard
        //EFFECTS: burns the next card
        void burn();

        //MODIFIES: players, numPlayers, dealer
        //EFFECTS: kicks out any player who's out of chips
        void eliminate();

        //MODIFIES: game state, players
        //EFFECTS: resets everything for the next hand
        void reset();

        //MODIFIES: game state, players
        //EFFECTS: posts the blinds and runs the preflop betting round
        void preFlop();

        //MODIFIES: game state, players
        //EFFECTS: runs the flop betting round and deals the turn
        void flop();

        //MODIFIES: game state, players
        //EFFECTS: runs the turn betting round and deals the river
        void turn();

        //MODIFIES: game state, players
        //EFFECTS: runs the river betting round
        void river();

        //MODIFIES: game state, players
        //EFFECTS: goes around the table, starting at current, until betting's done
        void round(int current);

        //MODIFIES: the last remaining player's chips
        //EFFECTS: pays the whole pot to whoever's left when everyone else has folded
        void payLastPlayer();

        //EFFECTS: the minimum legal raise amount
        int getMinRaise();

        //EFFECTS: which actions the player can legally choose right now
        std::vector<Action> getAvailableActions(Player &player);

        //EFFECTS: true if the player can make a legal raise
        bool playerCanRaise(const Player &player);

        //EFFECTS: how many live players (other than this one) still need to
        //         act this betting round
        int playersYetToAct(const Player &player);

        //MODIFIES: player, currentHighestBet, lastRaiseSize, pot, playersInHand
        //EFFECTS: applies the player's action and updates the betting state to match
        void applyAction(Player &player, const PlayerAction &action);

        //EFFECTS: true if the current betting round is done
        bool isBettingDone();

        //MODIFIES: players
        //EFFECTS: builds the main pot and any side pots and pays out each one's winner(s)
        void decideWinner();

        //MODIFIES: winners
        //EFFECTS: splits one pot evenly among its winners, then hands out any
        //         leftover odd chips clockwise starting from the dealer
        void payWinner(const std::vector<Player*> &winners, int potAmount);

        //EFFECTS: true if only one player is left in the hand
        bool oneRemaining();

        //EFFECTS: true if more than one player could still voluntarily bet
        //         this hand (not folded, not already all-in) - once at most
        //         one such player is left, there's no one left who could
        //         respond to a bet, so the rest of the hand just gets dealt
        //         out instead of asking that lone player for a pointless action
        bool bettingPossible();

        //MODIFIES: currentHighestBet, lastRaiseSize, players
        //EFFECTS: resets the betting state for the next street
        void resetBettingRound();

        //EFFECTS: the player's position relative to the dealer (button, cutoff, etc.)
        Position getPosition(int playerIndex) const;
};