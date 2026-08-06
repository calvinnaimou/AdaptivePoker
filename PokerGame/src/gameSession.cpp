#include "gameSession.h"

#include "calculator.h"

void refreshSnapshot(GameSnapshot &snapshot, const Game &game, bool revealAll){
    std::lock_guard<std::mutex> lock(snapshot.mutex);

    snapshot.board = game.getBoard();
    snapshot.pot = game.getPot();
    snapshot.dealer = game.getDealer();

    snapshot.players.clear();
    for(const auto &player : game.getPlayers()){
        bool isHuman = dynamic_cast<const GuiHuman*>(player.get()) != nullptr;

        PlayerView view;
        view.id = player->getID();
        view.name = player->getName();
        view.chips = player->getNumChips();
        view.currentBet = player->getCurrentBet();
        view.folded = player->folded();
        view.allIn = player->allIn();
        view.isHuman = isHuman;
        view.hand = player->getHand();
        view.handRevealed = isHuman || (revealAll && !player->folded());

        snapshot.players.push_back(view);
    }
}

void refreshHandOdds(GameSnapshot &snapshot, const Game &game){
    const Player* human = nullptr;
    for(const auto &player : game.getPlayers()){
        if(dynamic_cast<const GuiHuman*>(player.get()) != nullptr){
            human = player.get();
            break;
        }
    }

    if(human == nullptr || human->folded()){
        std::lock_guard<std::mutex> lock(snapshot.mutex);
        snapshot.handOddsValid = false;
        return;
    }

    int boardSize = static_cast<int>(game.getBoard().size());
    {
        std::lock_guard<std::mutex> lock(snapshot.mutex);
        if(snapshot.handOddsValid && snapshot.handOddsBoardSize == boardSize){
            return;
        }
    }

    //runs without holding snapshot's mutex, so the render thread can keep
    //reading everything else while this (potentially slow, on an empty
    //preflop board) brute force works
    Calculator calc(human->getHand(), game.getBoard());
    calc.calculate();

    std::array<double, 10> odds{
        calc.highCardOdds(), calc.onePairOdds(), calc.twoPairOdds(), calc.tripsOdds(),
        calc.straightOdds(), calc.flushOdds(), calc.fullHouseOdds(), calc.quadsOdds(),
        calc.straightFlushOdds(), calc.royalFlushOdds()
    };

    std::lock_guard<std::mutex> lock(snapshot.mutex);
    snapshot.handOdds = odds;
    snapshot.handOddsValid = true;
    snapshot.handOddsBoardSize = boardSize;
}

std::string describeAction(const Player &player, const PlayerAction &action, bool reopenedBetting){
    switch(action.action){
        case Action::Fold:
            return "Fold";
        case Action::Check:
            return "Check";
        case Action::Call:
            return "Call";
        case Action::Raise:
            return "Raise to " + std::to_string(player.getCurrentBet());
        case Action::AllIn:
            if(reopenedBetting){
                return "All In to " + std::to_string(player.getCurrentBet());
            }
            return "All In";
    }

    return "";
}

GuiHuman::GuiHuman(const std::string &nameIn, int chipsIn, int playerIDin, GameSnapshot *snapshotIn)
    : Player(nameIn, chipsIn, playerIDin), snapshot(snapshotIn){}

PlayerAction GuiHuman::act(const BasicCPUState& basicState){
    std::unique_lock<std::mutex> lock(snapshot->mutex);

    snapshot->availableActions = basicState.availableActions;
    snapshot->minRaise = basicState.minRaise;
    snapshot->maxRaise = basicState.maxRaise;
    snapshot->decisionReady = false;
    snapshot->awaitingHumanAction = true;

    snapshot->cv.wait(lock, [&]{ return snapshot->decisionReady; });

    snapshot->awaitingHumanAction = false;
    return snapshot->decision;
}
