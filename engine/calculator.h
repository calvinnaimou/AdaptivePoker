#pragma once
#include <array>
#include <cstddef>
#include <vector>

#include "card.h"

class Calculator {
    private:
        std::array<Card, 2> hand;
        std::vector<Card> board;
        std::vector<Card> unrevealed;
        std::vector<int> counter;
        std::vector<Card> chosen;
        int totalHands;

    public:
        //MODIFIES: hand, board, unrevealed, counter, chosen, totalHands
        //EFFECTS: takes the known cards and builds the deck of what's left to be dealt
        Calculator(const std::array<Card, 2>& handIn,
                   const std::vector<Card>& boardIn);

        //MODIFIES: counter, chosen, totalHands
        //EFFECTS: brute-forces every possible remaining board and tallies
        //         how many land in each hand category
        void calculate();

        //MODIFIES: counter, chosen
        //EFFECTS: recursively builds every possible final board, starting
        //         from unrevealed[ind]
        void generateBoards(std::size_t ind);

        //below: the exact % chance of ending up with each hand category -
        //these all add up to 100% across the full set

        //EFFECTS: % chance of ending up with no better than high card
        double highCardOdds() const;

        //EFFECTS: % chance of ending up with exactly one pair
        double onePairOdds() const;

        //EFFECTS: % chance of ending up with two pair
        double twoPairOdds() const;

        //EFFECTS: % chance of ending up with trips
        double tripsOdds() const;

        //EFFECTS: % chance of ending up with a straight
        double straightOdds() const;

        //EFFECTS: % chance of ending up with a flush
        double flushOdds() const;

        //EFFECTS: % chance of ending up with a full house
        double fullHouseOdds() const;

        //EFFECTS: % chance of ending up with quads
        double quadsOdds() const;

        //EFFECTS: % chance of ending up with a straight flush
        double straightFlushOdds() const;

        //EFFECTS: % chance of ending up with a royal flush
        double royalFlushOdds() const;
};

//EFFECTS: n choose r
long long nChooseR(int n, int r);