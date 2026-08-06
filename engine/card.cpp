#include "card.h"

constexpr const char *const RankNames[]{
    "Two",
    "Three",
    "Four",
    "Five",
    "Six",
    "Seven",
    "Eight",
    "Nine",
    "Ten",
    "Jack",
    "Queen",
    "King",
    "Ace"
};

constexpr const char* const SuitNames[]{
    "Spades",
    "Clubs",
    "Diamonds",
    "Hearts"
};

std::ostream& operator<<(std::ostream &os, Rank rank){
    //ranks start at 2, but the name list starts at index 0
    os << RankNames[static_cast<int>(rank) - 2];
    return os;
}

std::ostream& operator<<(std::ostream &os, Suit suit){
    os << SuitNames[static_cast<int>(suit)];
    return os;
}

std::ostream& operator<<(std::ostream &os, const Card &card){
    os << card.rank << " of " << card.suit;
    return os;
}

bool operator==(const Card &c1, const Card &c2){
    return c1.rank == c2.rank && c1.suit == c2.suit;
}