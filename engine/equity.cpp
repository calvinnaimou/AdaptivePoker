#include <algorithm>
#include <unordered_set>

#include "equity.h"
#include "card.h"
#include "handEval.h"

Equity::Equity(const std::array<Card, 2>& handIn, const std::vector<Card>& boardIn,
                                      int numSimulationsIn, int numOpponentsIn):
      board(boardIn), hand(handIn), numSimulations(numSimulationsIn),
      numOpponents(numOpponentsIn), generator(std::random_device{}()){

    unrevealed.reserve(52 - hand.size() - board.size());

    boardSize = static_cast<int>(board.size());

    std::unordered_set<Card, CardHash> revealed;

    for(const Card& card : hand){
        revealed.insert(card);
    }

    for(const Card& card : board){
        revealed.insert(card);
    }

    //pad out to all 5 board slots now, even the ones not dealt yet, so each
    //simulation can just write into board[boardSize..4] by index
    board.resize(5);

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

void Equity::dealPlayers(std::vector<std::array<Card, 2>> &opponentHands){
    for(int opp = 0; opp < numOpponents; ++opp){
        for(int j = 0; j < 2; ++j){
            opponentHands[opp][j] = unrevealed[nextCard--];
        }
    }
}

void Equity::dealBoard(){
    for(int i = boardSize; i < 5; ++i){
        board[i] = unrevealed[nextCard--];
    }
}

EquityResult Equity::calculate(){
    //result accumulates via decideWinner()'s win/tie/lossRate counters below,
    //so this needs to start from zero each time - otherwise calling
    //calculate() again on the same Equity instance would pile the new run on
    //top of the last one's already-averaged numbers
    result = EquityResult{};

    double totalPotShare = 0.0;

    //reused across every simulation so it's not reallocated each time
    std::vector<std::array<Card, 2>> opponentHands(numOpponents);

    for(int i = 0; i < numSimulations; ++i){
        //shuffle once and deal off the top - cheaper than re-picking random
        //cards each time and just as fair
        std::shuffle(unrevealed.begin(), unrevealed.end(), generator);
        nextCard = static_cast<int>(unrevealed.size()) - 1;
        dealPlayers(opponentHands);
        dealBoard();
        double share = decideWinner(opponentHands);
        totalPotShare += share;
    }

    result.equity = totalPotShare / numSimulations;
    result.winRate = result.winRate / numSimulations;
    result.lossRate = result.lossRate / numSimulations;
    result.tieRate = result.tieRate / numSimulations;
    return result;
}

double Equity::decideWinner(const std::vector<std::array<Card, 2>>& opponentHands){
    int numWinners = 1;//starts at 1 assuming our hand is the best so far

    HandEval playerEval(hand, board);
    HandValue playerValue = playerEval.evalValue(playerEval.evalCategory());
    HandValue bestValue = playerValue;

    for(const std::array<Card, 2>& opponentHand : opponentHands){
        HandEval opponentEval(opponentHand, board);
        HandValue opponentValue =
            opponentEval.evalValue(opponentEval.evalCategory());

        if(opponentValue > bestValue){
            bestValue = opponentValue;
            numWinners = 1;
        }
        else if(opponentValue == bestValue){
            ++numWinners;
        }
    }

    if(playerValue == bestValue){//win or tie
        if(numWinners == 1){//player won
            ++result.winRate;
        }
        else{//player tied
            ++result.tieRate;
        }
        return 1.0 / numWinners;
    }
    ++result.lossRate;
    return 0.0;//loss
}