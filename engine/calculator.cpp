#include <algorithm>
#include <unordered_set>

#include "calculator.h"
#include "handEval.h"


Calculator::Calculator(const std::array<Card, 2>& handIn,
                       const std::vector<Card>& boardIn):
      hand(handIn), board(boardIn), chosen(boardIn), totalHands(0){

    unrevealed.reserve(52 - hand.size() - board.size());

    counter.resize(10, 0);

    std::unordered_set<Card, CardHash> revealed;

    for(const Card& card : hand){
        revealed.insert(card);
    }

    for(const Card& card : board){
        revealed.insert(card);
    }

    //build the deck out of whatever's left once the hand and board are set aside
    for(Rank rank: allRanks){
        for(Suit suit: allSuits){
            Card card{rank, suit};
            if(revealed.count(card)) {
                continue;
            }
            unrevealed.push_back(card);
        }
    }
}

void Calculator::calculate(){
    std::fill(counter.begin(), counter.end(), 0);
    generateBoards(0);
    totalHands = nChooseR((50 - board.size()), 5 - board.size());
}

void Calculator::generateBoards(std::size_t cardInd){
    if(chosen.size() == 5){
        HandEval eval(hand, chosen);
        HandCategory category = eval.evalCategory();

        ++counter[static_cast<int>(category)];

        return;
    }

    std::size_t cardsNeeded = 5 - chosen.size();

    //cardInd could be bigger than unrevealed.size() by the time we get this
    //deep, so check before subtracting or this underflows (both are unsigned)
    if(cardsNeeded > unrevealed.size() - cardInd){
        return;
    }

    //stop far enough short of the end that there's always enough left to finish the board
    for(std::size_t i = cardInd; i <= unrevealed.size() - cardsNeeded; ++i){
        chosen.push_back(unrevealed[i]);
        generateBoards(i + 1);
        chosen.pop_back();
    }
}

//below: each *Odds() function just reads one slot out of counter - the name
//says which hand category, so these don't need their own comment repeating it

double Calculator::highCardOdds() const{
    return 100 * static_cast<double>(
        counter[static_cast<int>(HandCategory::highCard)]
    ) / totalHands;
}

double Calculator::onePairOdds() const{
    return 100 * static_cast<double>(
        counter[static_cast<int>(HandCategory::onePair)]
    ) / totalHands;
}

double Calculator::twoPairOdds() const{
    return 100 * static_cast<double>(
        counter[static_cast<int>(HandCategory::twoPair)]
    ) / totalHands;
}

double Calculator::tripsOdds() const{
    return 100 * static_cast<double>(
        counter[static_cast<int>(HandCategory::trips)]
    ) / totalHands;
}

double Calculator::straightOdds() const{
    return 100 * static_cast<double>(
        counter[static_cast<int>(HandCategory::straight)]
    ) / totalHands;
}

double Calculator::flushOdds() const{
    return 100 * static_cast<double>(
        counter[static_cast<int>(HandCategory::flush)]
    ) / totalHands;
}

double Calculator::fullHouseOdds() const{
    return 100 * static_cast<double>(
        counter[static_cast<int>(HandCategory::fullHouse)]
    ) / totalHands;
}

double Calculator::quadsOdds() const{
    return 100 * static_cast<double>(
        counter[static_cast<int>(HandCategory::quads)]
    ) / totalHands;
}

double Calculator::straightFlushOdds() const{
    return 100 * static_cast<double>(
        counter[static_cast<int>(HandCategory::straightFlush)]
    ) / totalHands;
}

double Calculator::royalFlushOdds() const{
    return 100 * static_cast<double>(
        counter[static_cast<int>(HandCategory::royalFlush)]
    ) / totalHands;
}

long long nChooseR(int n, int r){
    if(r < 0 || r > n){
        return 0;
    }

    r = std::min(r, n - r);

    long long result = 1;

    for(int i = 1; i <= r; ++i){
        result = result * (n - r + i) / i;
    }

    return result;
}