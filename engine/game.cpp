#include <algorithm>
#include <unordered_map>

#include "game.h"
#include "handEval.h"

std::string actionToString(Action action){
    switch(action){
        case(Action::AllIn):
            return "All-In";
        case(Action::Call):
            return "Call";
        case(Action::Check):
            return "Check";
        case(Action::Fold):
            return "Fold";
        case(Action::Raise):
            return "Raise";
    }
}

void Game::shuffle(){
    std::shuffle(deck.begin(), deck.end(), rng);
}

Game::Game(std::vector<std::unique_ptr<Player>> playersIn, int /*buyIn*/,
                                   int smallBlindIn, int bigBlindIn)
   :players(std::move(playersIn)){
    //build a standard 52-card deck
    deck.reserve(52);
    for(int suit = 0; suit <= 3; ++suit){
        for(int rank = 2; rank <= 14; ++rank){
            deck.emplace_back(Card{static_cast<Rank>(rank),
                                  static_cast<Suit>(suit)});
        }
    }
    numPlayers = players.size();
    smallBlindCost = smallBlindIn;
    bigBlindCost = bigBlindIn;

    //CPUs use this to gauge pot size in big blinds rather than raw chips
    players[0]->bigBlind = bigBlindIn;
    reset();
}

void Game::simulate(){
    while(!gameOver){
        dealPlayers();
        if(onHandStart) onHandStart(*this);

        preFlop();
        resetBettingRound();
        flop();
        resetBettingRound();
        turn();
        resetBettingRound();
        river();

        bool showdown = !oneRemaining();
        if(oneRemaining()){
            payLastPlayer();
        }
        else{
            decideWinner();
        }

        if(onHandEnd) onHandEnd(*this, showdown);

        reset();
    }
}

const std::vector<std::unique_ptr<Player>>& Game::getPlayers() const{
    return players;
}

const std::vector<Card>& Game::getBoard() const{
    return board;
}

int Game::getPot() const{
    return pot;
}

int Game::getDealer() const{
    return dealer;
}

void Game::dealPlayers(){
    for(int i = 0;  i < 2; ++i){
        //start left of the dealer, wrap around and finish on the dealer
        for(int j = 1; j <= numPlayers; ++j){
            Player &current = *(players[(dealer + j) % numPlayers]);
            current.addCard(deck[nextCard--], i);
        }
    }
}

void Game::dealFlop(){
    burn();
    for(int i = 0; i < 3; ++i){
        board.push_back(deck[nextCard--]);
    }
}

void Game::dealTurn(){
    burn();
    board.push_back(deck[nextCard--]);
}

void Game::dealRiver(){
    burn();
    board.push_back(deck[nextCard--]);
}

void Game::burn(){
    --nextCard;
}

void Game::eliminate(){
    for(int i = numPlayers - 1; i >= 0; --i){
        if(players[i]->getNumChips() == 0){
            //losing the dealer (or anyone before them) shifts the button left
            if(i <= dealer){
                --dealer;
            }

            players.erase(players.begin() + i);
        }
    }

    numPlayers = static_cast<int>(players.size());

    //dealer wrapped past the start - put it back at the end
    if(dealer < 0){
        dealer = numPlayers - 1;
    }
}

void Game::reset(){
    //counts up to "the hand about to be played" (1-indexed) - stamped onto
    //BasicCPUState in round() below so a shared observer (see AdvancedCPU's
    //hands-seen-per-opponent tracking) can tell how many distinct hands an
    //opponent has actually been seen playing, not just how many of some
    //narrower action type they've taken
    ++totalHands;

    eliminate();

    if(numPlayers == 1){
        gameOver = true;
        if(onGameOver) onGameOver(*this, *players[0]);
        return;
    }

    shuffle();
    dealer = (dealer + 1) % numPlayers;
    pot = 0;
    currentHighestBet = bigBlindCost;
    lastRaiseSize = bigBlindCost;
    playersInHand = numPlayers;
    callersThisStreet = 0;
    board.clear();
    nextCard = 51;
    for(int i = 0; i < numPlayers; ++i){
        players[i]->resetForNewHand();
    }
}

void Game::preFlop(){
    //with 3+ players, the blinds are the two seats left of the dealer;
    //heads-up, the dealer posts small blind and acts first instead
    Player &smallBlind = numPlayers > 2 ? *(players[(dealer + 1) % numPlayers])
                                        : *(players[dealer]);
    Player &bigBlind = numPlayers > 2 ? *(players[(dealer + 2) % numPlayers])
                                      : *(players[(dealer + 1) % numPlayers]);
    lastAggressor = &bigBlind;

    //each blind posts as much as they can afford, and may go all-in doing it
    int amount = std::min(smallBlind.getNumChips(), smallBlindCost);
    smallBlind.addToCurrentBet(amount);

    amount = std::min(bigBlind.getNumChips(), bigBlindCost);
    bigBlind.addToCurrentBet(amount);

    pot += bigBlind.getCurrentBet() + smallBlind.getCurrentBet();

    int firstToAct = numPlayers > 2
                    ? (dealer + 3) % numPlayers
                    : dealer;

    round(firstToAct);

    if(oneRemaining()){
        return;
    }

    dealFlop();
}

//flop/turn/river all follow the same shape: skip the betting round once
//bettingPossible() says nobody left could actually respond to a bet, but
//still deal the street's card(s) either way. preFlop() above is the one
//exception - the last player standing there might still owe a genuine
//first decision (e.g. call-or-fold an all-in shove) they haven't gotten
//the chance to make yet, so it isn't guarded the same way

void Game::flop(){
    if(oneRemaining()) return;
    if(bettingPossible()){
        round((dealer + 1) % numPlayers);
    }

    if(oneRemaining()){
        return;
    }

    dealTurn();
}

void Game::turn(){
    if(oneRemaining()) return;
    if(bettingPossible()){
        round((dealer + 1) % numPlayers);
    }

    if(oneRemaining()){
        return;
    }

    dealRiver();
}

void Game::river(){
    if(oneRemaining()) return;
    if(bettingPossible()){
        round((dealer + 1) % numPlayers);
    }
}

void Game::round(int current){
    //counts up across the whole game, never reset - stamped onto each
    //decision's BasicCPUState so a shared observer (see
    //AdvancedCPU::actionHistory) can tell "the same action, seen once per
    //player object at the table" apart from two genuinely different actions
    static int actionSeq = 0;

    while(true){
        Player &player = *(players[current]);

        //folded and all-in players have nothing left to decide
        if(!player.allIn() && !player.folded()){
            std::vector<Action> actions = getAvailableActions(player);
            int minRaise = getMinRaise();
            int maxRaise = player.getNumChips() - (currentHighestBet - player.getCurrentBet());
            Position position = getPosition(current);

            BasicCPUState state{
                actions,
                board,
                lastAggressor,
                pot,
                currentHighestBet,
                minRaise,
                maxRaise,
                playersInHand,
                callersThisStreet,
                playersYetToAct(player),
                position,
                ++actionSeq,
                totalHands
            };

            if(onTurnStart) onTurnStart(*this, player);

            PlayerAction action = player.act(state);
            applyAction(player, action);

            //let every player (not just the actor) build opponent-modeling
            //stats off this public action - state is the table as it stood
            //right before the action, matching how it was built above; note
            //applyAction() has already updated player's own chip totals by
            //now, so player.getCurrentBet() reflects the result while
            //everything else in state is still "before"
            for(const auto &p : players){
                p->observeAction(player, action, state);
            }
        }

        current = (current + 1) % numPlayers;
        if(isBettingDone()) break;
    }
}

void Game::payLastPlayer(){
    for(int i = 0; i < numPlayers; ++i){
        Player &player = *players[i];
        if(!player.folded()){
            player.addChips(pot);
            return;
        }
    }
}

int Game::getMinRaise(){
    return lastRaiseSize;
}

std::vector<Action> Game::getAvailableActions(Player &player){
    std::vector<Action> actions;

    int amountToBet = currentHighestBet - player.getCurrentBet();
    int chips = player.getNumChips();

    if(amountToBet > 0){
        actions.push_back(Action::Fold);

        if(chips > amountToBet){
            actions.push_back(Action::Call);
        }
    }
    else{
        actions = {Action::Check};
    }

    if (playerCanRaise(player)) {
        actions.push_back(Action::Raise);
    }

    //covers a short-stacked all-in call, or shoving while still able to
    //raise - both are legal any time this function runs, since a player
    //never gets asked to act twice in the same raise cycle (see
    //playerCanRaise's comment on hasActedSinceLastRaise)
    if(chips <= amountToBet || !player.hasActedSinceLastRaise()){
        actions.push_back(Action::AllIn);
    }

    return actions;
}

bool Game::playerCanRaise(const Player &player){
    int amountToCall = currentHighestBet - player.getCurrentBet();
    //hasActedSinceLastRaise is always false whenever this actually runs for
    //the player on the clock - round() only ever calls act() for someone who
    //hasn't responded to the current price yet - so this check is really
    //just documenting that invariant, not doing any real filtering here
    return player.getNumChips() >= amountToCall + lastRaiseSize && !player.hasActedSinceLastRaise();
}

int Game::playersYetToAct(const Player &player){
    int count = 0;

    for(const auto &otherPlayer : players){
        if(otherPlayer.get() == &player || otherPlayer->folded() || otherPlayer->allIn()){
            continue;
        }

        if(!otherPlayer->hasActedSinceLastRaise()){
            ++count;
        }
    }

    return count;
}

void Game::applyAction(Player &player, const PlayerAction &action){
    //only a full legal raise reopens the betting round for everyone else
    bool bettingReopened = false;

    switch(action.action){
        case Action::Raise:{
            //a Raise always reopens betting for everyone else (see below),
            //so anyone who already called is now calling a stale, lower
            //price and needs to be cleared - true even for a bare
            //minimum-sized raise (action.amount == lastRaiseSize), not just
            //a bigger one
            callersThisStreet = 0;

            lastRaiseSize = action.amount;
            currentHighestBet += action.amount;

            //what the player owes: the call, plus the raise on top
            int total = currentHighestBet - player.getCurrentBet();

            player.addToCurrentBet(total);
            pot += total;
            bettingReopened = true;
            lastAggressor = &player;
            break;
        }

        case Action::Call:{
            int call = currentHighestBet - player.getCurrentBet();

            player.addToCurrentBet(call);
            pot += call;
            ++callersThisStreet;
            break;
        }

        case Action::AllIn:{
            int bet = player.getNumChips();
            int newPlayerBet = player.getCurrentBet() + bet;

            if(newPlayerBet > currentHighestBet){
                int raise = newPlayerBet - currentHighestBet;
                bool fullRaise = raise >= lastRaiseSize;

                //a shove that falls short of a full raise still bumps the
                //price up, but doesn't reopen betting - players who already
                //called just owe the extra top-up, so they're still callers
                //and callersThisStreet is left alone. A full-sized shove
                //reopens betting for everyone (bettingReopened below), so it
                //clears out stale callers of the old price the same way a
                //plain Raise does, including one sized at exactly the minimum
                if(fullRaise){
                    lastRaiseSize = raise;
                    bettingReopened = true;
                    callersThisStreet = 0;
                }

                currentHighestBet = newPlayerBet;
                lastAggressor = &player;
            }
            else{
                //going all-in for less than the current bet is really just a call
                ++callersThisStreet;
            }

            pot += bet;
            player.addToCurrentBet(bet);
            break;
        }

        case Action::Fold:{
            player.fold();
            --playersInHand;
            break;
        }

        case Action::Check:{
            break;
        }
    }
    player.setActedSinceLastRaise(true);

    if(bettingReopened){
        for(int i = 0; i < numPlayers; ++i){
            Player &p = *players[i];

            //everyone else still live needs to respond to the new raise
            if(&p != &player && !p.folded() && !p.allIn()){
                p.setActedSinceLastRaise(false);
            }
        }
    }

    if(onAction) onAction(*this, player, action, bettingReopened);
}

bool Game::isBettingDone(){
    if(oneRemaining()) return true;
    for(int i = 0; i < numPlayers; ++i){
        Player &player = *players[i];

        if(player.folded() || player.allIn()) continue;

        if(player.getCurrentBet() < currentHighestBet){
            return false;
        }

        if(!player.hasActedSinceLastRaise()){
            return false;
        }
    }
    return true;
}

void Game::decideWinner(){
    //score every non-folded hand exactly once up front, even though the
    //side-pot loop below might otherwise need the same hand's value more
    //than once - this also lets observeShowdown notify everyone about each
    //shower exactly once, after every pot layer is already paid out
    std::unordered_map<Player*, HandValue> handValues;
    std::unordered_map<Player*, bool> wonAnyPot;

    for(const auto &player : players){
        if(player->folded()) continue;

        HandEval handEval(player->getHand(), board);
        HandCategory category = handEval.evalCategory();
        handValues[player.get()] = handEval.evalValue(category);
        wonAnyPot[player.get()] = false;
    }

    std::vector<int> contributionLevels;

    for(const auto &player : players){
        int contribution = player->getTotalChipsInHand();

        if(contribution > 0){
            contributionLevels.push_back(contribution);
        }
    }

    //each distinct contribution amount marks the top of one pot layer - this
    //is the standard way to build a main pot plus side pots: a player who's
    //all-in for less can only compete for chips up to their own contribution
    std::sort(contributionLevels.begin(), contributionLevels.end());
    contributionLevels.erase(
        std::unique(contributionLevels.begin(), contributionLevels.end()),
        contributionLevels.end()
    );

    //everything below this has already gone into an earlier pot layer
    int previousLevel = 0;

    for(int currentLevel : contributionLevels){
        int contributors = 0;

        for(const auto &player : players){
            if(player->getTotalChipsInHand() >= currentLevel){
                ++contributors;
            }
        }

        int potAmount = (currentLevel - previousLevel) * contributors;

        std::vector<Player*> eligiblePlayers;

        //walking clockwise from the dealer keeps the eventual odd-chip
        //payout below in the right order
        for(int offset = 1; offset <= numPlayers; ++offset){
            int index = (dealer + offset) % numPlayers;
            Player *player = players[index].get();

            if(!player->folded() &&
               player->getTotalChipsInHand() >= currentLevel){
                eligiblePlayers.push_back(player);
            }
        }

        std::vector<Player*> winners;
        HandValue bestHand{};

        for(Player *player : eligiblePlayers){
            HandValue hand = handValues[player];

            if(hand > bestHand){
                bestHand = hand;
                winners.clear();
                winners.push_back(player);
            }
            else if(hand == bestHand){
                winners.push_back(player);
            }
        }

        for(Player *winner : winners){
            wonAnyPot[winner] = true;
        }

        payWinner(winners, potAmount);
        previousLevel = currentLevel;
    }

    //tell everyone what each shower had, once per shower, no matter how
    //many pot layers they were actually eligible for
    for(const auto &shower : players){
        auto it = handValues.find(shower.get());
        if(it == handValues.end()) continue;

        for(const auto &observer : players){
            observer->observeShowdown(*shower, board, it->second, wonAnyPot[shower.get()]);
        }
    }
}

void Game::payWinner(const std::vector<Player*> &winners, int potAmount){
    int n = static_cast<int>(winners.size());
    int split = potAmount / n;
    int remainder = potAmount % n;

    for(Player *winner : winners){
        winner->addChips(split);
    }

    //winners are already in clockwise order from the dealer, so the
    //leftover chips just go to the front of the list
    for(int i = 0; i < remainder; ++i){
        winners[i]->addChips(1);
    }
}

bool Game::oneRemaining(){
    return playersInHand == 1;
}

bool Game::bettingPossible(){
    int liveNotAllIn = 0;

    for(const auto &player : players){
        if(!player->folded() && !player->allIn()){
            ++liveNotAllIn;
        }
    }

    return liveNotAllIn > 1;
}

void Game::resetBettingRound(){
    currentHighestBet = 0;
    lastRaiseSize = bigBlindCost;
    callersThisStreet = 0;

    for(int i = 0; i < numPlayers; ++i){
        players[i]->resetForNewBettingRound();
    }
}

Position Game::getPosition(int playerIndex) const{
    int offset = (playerIndex - dealer + numPlayers) % numPlayers;

    if(offset == 0){
        return Position::Button;
    }

    //heads-up: the dealer (already returned as Button above) is also the
    //small blind, but plays that seat like a button strategically - they act
    //last postflop despite posting the small blind - so treating them as
    //Button rather than SmallBlind here is intentional. The only other seat
    //left is the big blind
    if(numPlayers == 2){
        return Position::BigBlind;
    }

    if(offset == 1){
        return Position::SmallBlind;
    }

    if(offset == 2){
        return Position::BigBlind;
    }

    //the seat right before the button
    if(offset == numPlayers - 1){
        return Position::Cutoff;
    }

    //hijack only shows up once the table's big enough to have one
    if(numPlayers >= 6 && offset == numPlayers - 2){
        return Position::Hijack;
    }

    //same idea for lojack
    if(numPlayers >= 7 && offset == numPlayers - 3){
        return Position::Lojack;
    }

    //everything left starts right after the big blind
    int utgOffset = offset - 3;

    if(utgOffset == 0){
        return Position::UnderTheGun;
    }

    if(utgOffset == 1){
        return Position::UnderTheGunPlus1;
    }

    if(utgOffset == 2){
        return Position::UnderTheGunPlus2;
    }

    return Position::UnderTheGunPlus3;
}
