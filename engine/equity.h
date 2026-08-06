#pragma once

#include <array>
#include <vector>
#include <random>
#include "card.h"

struct EquityResult{
    double equity; //overall share of the pot this hand is expected to win

    double winRate;
    double tieRate;
    double lossRate;
};

//Monte Carlo equity estimator: deals out a bunch of random opponent hands and
//boards, sees how often the given hand wins/ties/loses, and averages it out
class Equity{
    std::vector<Card> board;//cards already on the board
    std::array<Card, 2> hand;//the hand we're estimating equity for
    std::vector<Card> unrevealed;//everything still left in the deck
    EquityResult result{};
    int numSimulations;
    int numOpponents;

    std::mt19937 generator;//kept around so we're not reseeding every simulation

    int nextCard;//index of the next unrevealed card to deal
    int boardSize;//how many board cards were already known going in

    //MODIFIES: opponentHands
    //EFFECTS: deals each opponent their two cards for one simulation
    void dealPlayers(std::vector<std::array<Card, 2>> &opponentHands);

    //MODIFIES: board
    //EFFECTS: fills out whatever's left of the board for one simulation
    void dealBoard();
public:
    //MODIFIES: board, hand, unrevealed, numSimulations, numOpponents, generator
    //EFFECTS: sets up the known cards and builds the deck of what's left to deal
    Equity(const std::array<Card, 2>& handIn, const std::vector<Card>& boardIn,
                                      int numSimulationsIn, int numOpponentsIn);

    //MODIFIES: result
    //EFFECTS: runs all the simulations and returns the averaged-out equity
    EquityResult calculate();

    //MODIFIES: result
    //EFFECTS: scores one simulated showdown against result's running win/tie/loss
    //         counts, and returns this hand's share of that pot (1, a split, or 0)
    double decideWinner(const std::vector<std::array<Card, 2>>& opponentHands);
};