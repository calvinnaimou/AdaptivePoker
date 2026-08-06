#include "handEval.h"

HandEval::HandEval(const std::array<Card, 2> &hand, const std::vector<Card> &board){
    cards = {
        hand[0],
        hand[1],
        board[0],
        board[1],
        board[2],
        board[3],
        board[4]
    };

    for(const Card &card: cards){
        int rank = static_cast<int>(card.rank);
        int suit = static_cast<int>(card.suit);

        ++rankCount[rank];
        rankPresent[rank] = true;
        ++suitCount[suit];
        suitedRanks[suit][rank] = true;
    }

    //mirror the ace down to index 1 too, so a wheel (A-2-3-4-5) shows up
    //as a run when we scan for straights starting from the low end
    rankPresent[1] = rankPresent[14];
}

HandCategory HandEval::evalCategory(){
    //a royal flush is just a straight flush that happens to start at ten
    if(isStraightFlush()){
        if(straightStart == 10){
            return HandCategory::royalFlush;
        }

        return HandCategory::straightFlush;
    }

    //walking down from the ace means whatever we store for trips/pairs
    //naturally ends up highest-first, no separate sort needed
    for(int rank = 14; rank >= 2; --rank){
        if(rankCount[rank] == 4){
            quadRank = rank;
        }
        else if(rankCount[rank] == 3){
            tripRanks[numTrips++] = rank;
        }
        else if(rankCount[rank] == 2){
            pairRanks[numPairs++] = rank;
        }
    }

    if(quadRank != 0){
        return HandCategory::quads;
    }

    if(numTrips >= 2 || (numTrips >= 1 && numPairs >= 1)){
        return HandCategory::fullHouse;
    }

    if(isFlush()){
        return HandCategory::flush;
    }

    if(isStraight()){
        return HandCategory::straight;
    }

    if(numTrips == 1){
        return HandCategory::trips;
    }

    if(numPairs >= 2){
        return HandCategory::twoPair;
    }

    if(numPairs == 1){
        return HandCategory::onePair;
    }

    return HandCategory::highCard;
}

HandValue HandEval::evalValue(HandCategory category){
    handVal.category = category;

    switch(category){
        case HandCategory::highCard:
            findHighCard();
            break;
        case HandCategory::onePair:
            findOnePair();
            break;
        case HandCategory::twoPair:
            findTwoPair();
            break;
        case HandCategory::trips:
            findTrips();
            break;
        case HandCategory::straight:
            findStraight();
            break;
        case HandCategory::flush:
            findFlush();
            break;
        case HandCategory::fullHouse:
            findFullHouse();
            break;
        case HandCategory::quads:
            findQuads();
            break;
        case HandCategory::straightFlush:
            findStraightFlush();
            break;
        case HandCategory::royalFlush:
            findRoyalFlush();
            break;
    }

    return handVal;
}

bool HandEval::isStraightFlush(){
    for(int suit = 0; suit < 4; ++suit){
        if(suitCount[suit] < 5){
            continue;
        }

        int consecutive = 0;

        //scanning from the ace down means the first run of 5 we hit is the highest one
        for(int rank = 14; rank >= 2; --rank){
            if(suitedRanks[suit][rank]){
                ++consecutive;
                if(consecutive == 5){
                    straightStart = rank;
                    return true;
                }
            }
            else{
                consecutive = 0;
            }
        }

        //the ace only lives at index 14 here (unlike rankPresent, which
        //mirrors it to 1), so the wheel needs its own explicit check
        if(suitedRanks[suit][14] &&
           suitedRanks[suit][2] &&
           suitedRanks[suit][3] &&
           suitedRanks[suit][4] &&
           suitedRanks[suit][5]){
            straightStart = 1;
            return true;
        }
    }

    return false;
}

bool HandEval::isFlush(){
    for(int i = 0; i < 4; ++i){
        if(suitCount[i] >= 5){
            flushSuit = i;
            return true;
        }
    }

    return false;
}

bool HandEval::isStraight(){
    int counter = 0;

    //index 1 is the mirrored ace, so a wheel (5-4-3-2-A) reads as a normal run
    for(int i = 14; i >= 1; --i){
        if(rankPresent[i]){
            ++counter;
            if(counter == 5){
                straightStart = i;
                return true;
            }
        }
        else{
            counter = 0;
        }
    }

    return false;
}

void HandEval::findHighCard(){
    int ind = 0;

    for(int i = 14; i >= 2; --i){
        if(rankCount[i] == 1){
            handVal.ranks[ind++] = i;
            if(ind == 5) return;
        }
    }
}

void HandEval::findOnePair(){
    int ind = 2;
    handVal.ranks[0] = handVal.ranks[1] = pairRanks[0];

    for(int i = 14; i >= 2; --i){
        if(rankCount[i] == 1){
            handVal.ranks[ind++] = i;
            if(ind == 5){
                return;
            }
        }
    }
}

void HandEval::findTwoPair(){
    handVal.ranks[0] = handVal.ranks[1] = pairRanks[0];
    handVal.ranks[2] = handVal.ranks[3] = pairRanks[1];

    //kicker is the next-best card that isn't part of either pair
    for(int i = 14; i >= 2; --i){
        if(rankCount[i] >= 1 && i != pairRanks[0] && i != pairRanks[1]){
            handVal.ranks[4] = i;
            return;
        }
    }
}

void HandEval::findTrips(){
    int ind = 3;
    handVal.ranks[0] = handVal.ranks[1] = handVal.ranks[2] = tripRanks[0];

    for(int i = 14; i >= 2; --i){
        if(rankCount[i] == 1 && i != tripRanks[0]){
            handVal.ranks[ind++] = i;
            if(ind == 5){
                break;
            }
        }
    }
}

void HandEval::findStraight(){
    if(straightStart == 1){
        //the wheel - ranks it below every other straight
        handVal.ranks = {5, 4, 3, 2, 1};
        return;
    }

    for(int i = 0; i < 5; ++i){
        handVal.ranks[i] = straightStart + 4 - i;
    }
}

void HandEval::findFlush(){
    int ind = 0;

    for(int i = 14; i >= 2; --i){
        if(suitedRanks[flushSuit][i]){
            handVal.ranks[ind++] = i;
            if(ind == 5) break;
        }
    }
}

void HandEval::findFullHouse(){
    handVal.ranks[0] = handVal.ranks[1] = handVal.ranks[2] = tripRanks[0];

    //two sets of trips? the lower one just plays as the pair
    if(numTrips == 2){
        handVal.ranks[3] = handVal.ranks[4] = tripRanks[1];
    }
    else{
        handVal.ranks[3] = handVal.ranks[4] = pairRanks[0];
    }
}

void HandEval::findQuads(){
    for(int j = 0; j <= 3; ++j){
        handVal.ranks[j] = quadRank;
    }

    //kicker is just the best remaining card
    for(int i = 14; i >= 2; --i){
        if(rankCount[i] >= 1 && i != quadRank){
            handVal.ranks[4] = i;
            return;
        }
    }
}

void HandEval::findStraightFlush(){
    if(straightStart == 1){
        handVal.ranks = {5, 4, 3, 2, 1};
        return;
    }

    for(int i = 0; i < 5; ++i){
        handVal.ranks[i] = straightStart + 4 - i;
    }
}

void HandEval::findRoyalFlush(){
    //always the same 5 cards, so there's nothing to look up
    handVal.ranks = {14, 13, 12, 11, 10};
}