#pragma once

#include <cstddef>
#include <functional>
#include <ostream>

enum class Rank{
    TWO = 2,
    THREE = 3,
    FOUR = 4,
    FIVE = 5,
    SIX = 6,
    SEVEN = 7,
    EIGHT = 8,
    NINE = 9,
    TEN = 10,
    JACK = 11,
    QUEEN = 12,
    KING = 13,
    ACE = 14,
};

enum class Suit{
    SPADES,
    CLUBS,
    DIAMONDS,
    HEARTS,
};

constexpr std::array<Rank, 13> allRanks{
    Rank::TWO,
    Rank::THREE,
    Rank::FOUR,
    Rank::FIVE,
    Rank::SIX,
    Rank::SEVEN,
    Rank::EIGHT,
    Rank::NINE,
    Rank::TEN,
    Rank::JACK,
    Rank::QUEEN,
    Rank::KING,
    Rank::ACE
};

constexpr std::array<Suit, 4> allSuits{
    Suit::SPADES,
    Suit::CLUBS,
    Suit::DIAMONDS,
    Suit::HEARTS
};

struct Card{
    Rank rank;
    Suit suit;
};

struct CardComp{
    //EFFECTS: true if c1 is the lower-ranked card
    bool operator()(const Card &c1, const Card &c2){
        return c1.rank < c2.rank;
    }
};

struct CardHash{
    //EFFECTS: hashes a card off its rank and suit
    std::size_t operator()(const Card& card) const{
        return std::hash<int>{}(static_cast<int>(card.rank)) ^
               (std::hash<int>{}(static_cast<int>(card.suit)) << 1);
    }
};

//MODIFIES: os
//EFFECTS: prints the rank ("Ace", "King", etc.)
std::ostream& operator<<(std::ostream &os, Rank rank);

//MODIFIES: os
//EFFECTS: prints the suit ("Spades", "Hearts", etc.)
std::ostream& operator<<(std::ostream &os, Suit suit);

//MODIFIES: os
//EFFECTS: prints the card as "Rank of Suit"
std::ostream& operator<<(std::ostream &os, const Card &card);

//EFFECTS: true if two cards match rank and suit
bool operator==(const Card &c1, const Card &c2);