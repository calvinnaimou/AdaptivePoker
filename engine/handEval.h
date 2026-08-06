#pragma once

#include <array>
#include <vector>

#include "card.h"

enum class HandCategory{
    highCard,
    onePair,
    twoPair,
    trips,
    straight,
    flush,
    fullHouse,
    quads,
    straightFlush,
    royalFlush
};

struct HandValue{
    HandCategory category = HandCategory::highCard;
    std::array<int, 5> ranks{};

    //EFFECTS: returns whether this hand value is lower than other
    bool operator<(const HandValue &other) const{
        if(category != other.category){
            return category < other.category;
        }
        return ranks < other.ranks;
    }

    //EFFECTS: returns whether this hand value is equal to other
    bool operator==(const HandValue &other) const{
        return category == other.category &&
            ranks == other.ranks;
    }

    //EFFECTS: returns whether this hand value is greater than other
    bool operator>(const HandValue &other) const{
        return other < *this;
    }
};

class HandEval{
    private:
        std::array<int, 15> rankCount{};
        std::array<int, 4> suitCount{};
        std::array<bool, 15> rankPresent{};
        std::array<std::array<bool, 15>, 4> suitedRanks{};
        std::array<Card, 7> cards;
        HandValue handVal{};

        std::array<int, 2> tripRanks{};
        std::array<int, 3> pairRanks{};
        int quadRank = 0;
        int numTrips = 0;
        int numPairs = 0;
        int flushSuit;
        int straightStart;

    public:
        //MODIFIES: cards, rankCount, suitCount, rankPresent, suitedRanks
        //EFFECTS: takes the 2 hole cards + 5 board cards and tallies up
        //         rank/suit counts for the rest of the class to use
        HandEval(const std::array<Card, 2>& hand,
                 const std::vector<Card>& board);

        //MODIFIES: quadRank, tripRanks, pairRanks, numTrips, numPairs, flushSuit, straightStart
        //EFFECTS: figures out the best hand category these 7 cards make
        HandCategory evalCategory();

        //MODIFIES: handVal
        //EFFECTS: builds the full, tiebreak-comparable value for a hand
        //         already known to be of the given category
        HandValue evalValue(HandCategory category);

    private:
        //MODIFIES: straightStart
        //EFFECTS: true if there's a straight flush in here
        bool isStraightFlush();

        //MODIFIES: flushSuit
        //EFFECTS: true if there's a flush in here
        bool isFlush();

        //MODIFIES: straightStart
        //EFFECTS: true if there's a straight in here
        bool isStraight();

        //everything below assumes the hand is already known to be that category

        //MODIFIES: handVal
        //EFFECTS: grabs the 5 highest card ranks
        void findHighCard();

        //MODIFIES: handVal
        //EFFECTS: grabs the pair plus its 3 best kickers
        void findOnePair();

        //MODIFIES: handVal
        //EFFECTS: grabs the top 2 pairs plus a kicker
        void findTwoPair();

        //MODIFIES: handVal
        //EFFECTS: grabs the trips plus its 2 best kickers
        void findTrips();

        //MODIFIES: handVal
        //EFFECTS: lays out the straight's ranks, high to low
        void findStraight();

        //MODIFIES: handVal
        //EFFECTS: grabs the 5 highest cards of the flush suit
        void findFlush();

        //MODIFIES: handVal
        //EFFECTS: lays out the full house's trips and pair
        void findFullHouse();

        //MODIFIES: handVal
        //EFFECTS: lays out the quads plus a kicker
        void findQuads();

        //MODIFIES: handVal
        //EFFECTS: lays out the straight flush's ranks, high to low
        void findStraightFlush();

        //MODIFIES: handVal
        //EFFECTS: lays out the royal flush (always A-K-Q-J-10)
        void findRoyalFlush();
};