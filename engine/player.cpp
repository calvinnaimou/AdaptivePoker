#include <algorithm>
#include <cassert>
#include <cmath>
#include <random>
#include <string>

#include "player.h"
#include "game.h"
#include "equity.h"

//number of Monte Carlo trials each CPU's equity estimate is based on - at
//~5 opponents preflop (the most expensive case, since the whole board still
//has to be simulated) this costs roughly 300-350ms per decision, benchmarked
//against an unoptimized build to match how the CLI/GUI are actually compiled
inline constexpr int CPU_SIMULATIONS = 70000;

//reset value for BlufferCPU::state.recentBluffHands after firing a bluff -
//also doubles as the divisor in bluffChance()'s cooldown factor, so the
//hand right after a bluff sees the strongest reduction and it fades out
//over the following hand before returning to normal
inline constexpr int BLUFF_COOLDOWN_HANDS = 3;

// anonymous (unnamed) namespace
namespace{
    std::mt19937 cpuRng{std::random_device{}()};

    //a random value in [0, 1) - the dice roll behind every probabilistic CPU decision
    double randomChance(){
        return std::uniform_real_distribution<double>(0.0, 1.0)(cpuRng);
    }

    //number of Monte Carlo trials for preflopHandStrength()'s heads-up reference
    //equity - much cheaper than CPU_SIMULATIONS since it's a coarse screening
    //gate, not a live pot-odds decision, and heads-up equity converges faster
    inline constexpr int PREFLOP_SCREEN_SIMULATIONS = 8000;

    //the hand's win-equity heads-up against one random opponent - used only
    //for the preflop starting-hand screen, which needs a stable measure of
    //raw hand strength that doesn't care how many players are at the table.
    //This screen used to compare against ACTUAL multiway equity (vs however
    //many opponents are still in), but that number gets squeezed hard toward
    //50% as opponents fold out of a round - a solidly-above-average hand can
    //sit under 55% equity heads-up while the same hand 5-handed might be 25%.
    //A single fixed bar against that was either way too loose at a full
    //table or basically unreachable once folds thinned the field, even for a
    //genuinely good hand. Heads-up equity keeps a wide, table-size-
    //independent ~15%-85% spread, so it's a stable ruler for "is this hand
    //worth playing." Actual multiway equity still gets computed separately
    //and drives the real pot-odds call/raise decision once a hand clears this screen
    double preflopHandStrength(const std::array<Card, 2>& hand){
        Equity screenEquity(hand, {}, PREFLOP_SCREEN_SIMULATIONS, 1);
        return screenEquity.calculate().equity;
    }

    //a preflop equity estimate for the RAISE decision specifically - against
    //however many opponents have actually shown interest by calling this
    //street so far, plus one for whoever's still left to act, NOT against
    //literally every player dealt into the hand. It's the same "worst case:
    //everyone stays to the end" problem preflopHandStrength() solves for the
    //starting-hand screen above, just previously left unfixed here: treating
    //playersInHand-1 as the opponent count means an early-position open at a
    //full table gets judged against 9 phantom opponents who'll mostly fold
    //to it, which can push even a premium pair's margin negative before the
    //pot's grown big enough to make up for it. The field hasn't had the
    //chance to fold down to its real size yet, so the call-decision's fix
    //(heads-up equity) doesn't cover this case. Capped at playersInHand-1,
    //since that's the most opponents who could possibly still call
    double preflopRaiseEquity(const std::array<Card, 2>& hand, const BasicCPUState& basicState){
        int realisticOpponents = std::clamp(
            basicState.callersThisStreet + 1, 1, std::max(1, basicState.playersInHand - 1)
        );
        Equity raiseEquityCalc(hand, basicState.board, PREFLOP_SCREEN_SIMULATIONS, realisticOpponents);
        return raiseEquityCalc.calculate().equity;
    }

    //the equity floor needed to lead out - bet with nobody having bet yet
    //this street, a continuation bet included - instead of just checking.
    //Real leading/c-betting needs genuine hand strength or a deliberate
    //bluff, not just any equity above zero: margin (equity minus potOdds)
    //collapses to raw equity when there's nothing to call, since potOdds is
    //0, and almost any hand worth continuing with clears even a ~0.15
    //raiseBar - so without this floor, leading out looked close to automatic
    //every single time
    constexpr double OPENING_BET_EQUITY_FLOOR = 0.45;

    //a position-based multiplier on the preflop starting-hand floor. Early
    //position stays at the already-tight heads-up-equity baseline (1.0, no
    //further tightening - stacking a positional penalty on an already-tight
    //floor is what made most seats fold too much), and only late position
    //gets a mild discount - matching how real preflop ranges widen toward
    //the button, without making early position unplayably tight or late
    //position wildly loose by comparison
    double positionalLooseness(Position position){
        switch(position){
            case Position::UnderTheGun:      return 1.00;
            case Position::UnderTheGunPlus1: return 1.00;
            case Position::UnderTheGunPlus2: return 1.00;
            case Position::UnderTheGunPlus3: return 0.98;
            case Position::Lojack:           return 0.96;
            case Position::Hijack:           return 0.93;
            case Position::Cutoff:           return 0.90;
            case Position::Button:           return 0.87;
            case Position::SmallBlind:       return 0.96;
            case Position::BigBlind:         return 0.93;
        }
        return 1.0;
    }

    //an extra multiplier on the preflop starting-hand floor, applied only
    //when nobody's raised yet (currentHighestBet is still exactly the big
    //blind). Position - and hand-strength requirements generally - should
    //matter far less for a decision this cheap than for facing a real
    //raise: entering a pot at the minimum price with a decent hand is
    //low-risk from any seat, while calling an actual raise is a bigger
    //commitment that deserves the full position/strength bar, since it
    //usually means playing a bigger pot out of position against someone
    //who's shown real strength. Goes neutral (1.0) the moment
    //currentHighestBet rises above the big blind - a real raise should
    //tighten things back to the normal, undiscounted bar
    double openPriceLooseness(int currentHighestBet){
        if(Player::bigBlind <= 0){
            return 1.0;
        }
        if(currentHighestBet <= Player::bigBlind){
            return 0.90;
        }
        return 1.0;
    }

    //a value-bet size scaled to raw hand strength rather than edge over
    //breakeven, sampled from a range around each tier so the size is never
    //a single predictable constant
    int valueBetSize(double equity, int potSize){
        double low, high;
        if(equity >= 0.70)     { low = 0.60; high = 0.90; }
        else if(equity >= 0.40){ low = 0.40; high = 0.65; }
        else                   { low = 0.25; high = 0.45; }

        double potFraction = low + randomChance() * (high - low);
        return static_cast<int>(potSize * potFraction);
    }

    //the last aggressor's remaining stack as a multiple of the pot, or 1.0
    //(neutral) if there's no relevant aggressor to compare against
    double aggressorStackToPot(const BasicCPUState& basicState, const Player* self){
        if(basicState.lastAggressor == nullptr || basicState.lastAggressor == self){
            return 1.0;
        }
        return static_cast<double>(basicState.lastAggressor->getNumChips()) / basicState.potSize;
    }

    //what fraction of this player's live opponents have already called this
    //street - a table-size-relative way to measure "how multiway is this,"
    //since 7 callers means something very different at a 10-handed table
    //(most of the field could still fold) than at a 4-handed one (everyone
    //left has already shown they're staying)
    double calledFraction(const BasicCPUState& basicState){
        int otherPlayers = basicState.playersInHand - 1;
        if(otherPlayers <= 0){
            return 0.0;
        }
        return std::clamp(static_cast<double>(basicState.callersThisStreet) / otherPlayers, 0.0, 1.0);
    }

    //a multiplier applied to callMargin/raiseMargin - above 1.0 means the
    //CPU should demand more edge than usual to continue, below 1.0 means demand less
    double situationalDifficulty(const BasicCPUState& basicState, const Player* self,
                                 double riskTolerance, bool forRaise){
        //raising must get through every live caller's range; calling instead gains implied odds from them.
        //Scaled by calledFraction (relative to table size) rather than a flat per-caller increment, so a
        //crowded pot means the same thing whether it's 7 callers at a 10-handed table or 3 at a 4-handed one
        double callerFactor = forRaise
            ? 1.0 + 1.2 * calledFraction(basicState)
            : std::max(1.0 - 0.15 * basicState.callersThisStreet, 0.55);

        //fewer players still needing to act means less risk of getting raised after committing chips
        double positionFactor = std::clamp(0.85 + 0.05 * basicState.playersYetToAct, 0.85, 1.15);

        //a short-stacked aggressor plays a wider, more desperate range than a deep-stacked one
        double stackToPot = aggressorStackToPot(basicState, self);
        double stackFactor = std::clamp(0.75 + 0.15 * stackToPot, 0.75, 1.25);

        //how much of the player's own stack is already committed to this pot -
        //the discount only kicks in for genuinely small bets (up to half the
        //pot as it stood before this bet); a committed stack is no excuse to
        //call a pot-sized bet without the equity to back it up, it just means
        //a cheap continue shouldn't be an easy fold
        double committed = static_cast<double>(self->getTotalChipsInHand());
        double startingStack = committed + static_cast<double>(self->getNumChips());
        double commitmentRatio = committed / startingStack;

        int amountToCall = basicState.currentHighestBet - self->getCurrentBet();
        double priorPot = static_cast<double>(basicState.potSize - amountToCall);
        double betToPotRatio = priorPot > 0 ? static_cast<double>(amountToCall) / priorPot : 1.0;
        double smallBetRelief = std::clamp(1.0 - betToPotRatio / 0.5, 0.0, 1.0);

        double commitmentFactor = 1.0 - commitmentRatio * smallBetRelief * 0.6;

        double difficulty = callerFactor * positionFactor * stackFactor * commitmentFactor;

        //more risk-tolerant CPUs let the situation move the bar less
        difficulty = 1.0 + (difficulty - 1.0) * (1.0 - riskTolerance);

        //raised from 1.75 so a fully multiway pot (calledFraction near 1.0)
        //can still meaningfully push raiseBar higher instead of saturating
        //against the ceiling well before the caller signal maxes out
        return std::clamp(difficulty, 0.5, 2.5);
    }

    //which street the given board corresponds to
    BettingStreet streetFromBoard(const std::vector<Card>& board){
        switch(board.size()){
            case 0:  return BettingStreet::PreFlop;
            case 3:  return BettingStreet::Flop;
            case 4:  return BettingStreet::Turn;
            default: return BettingStreet::River;
        }
    }

    //base chance of slowplaying (checking/calling instead of raising a
    //raise-worthy hand) on each street - highest on the flop, where there
    //are still two streets left to build a pot on later. Turn/river were
    //originally tuned much lower under "no future street left to wait for,
    //a hand good enough to raise should just go ahead and get value now" -
    //true for genuinely nutted hands, but a standalone street-by-street
    //diagnostic showed that reasoning, applied uniformly to every raise-
    //worthy hand (not just the nuts), collapsed flat-calling from a balanced
    //~50% on the flop down to just 24-35% on the river (65-76% raise) purely
    //from this schedule, not from the underlying equity-vs-bar math - visible
    //to a user as the river specifically (the final, highest-stakes street)
    //feeling like the CPU "always reraises." A merely strong (not nutted)
    //hand still has a real reason to sometimes just call the river instead of
    //raising (polarizing a value-only raise gets looked up by worse or folds
    //out better, the same reason real players don't raise every river value
    //hand) - raised turn/river enough to bring their flat-call rate back into
    //a comparable range to flop/preflop, while keeping a mild taper intact
    constexpr double PREFLOP_SLOWPLAY_CHANCE = 0.25;
    constexpr double FLOP_SLOWPLAY_CHANCE = 0.30;
    constexpr double TURN_SLOWPLAY_CHANCE = 0.28;
    constexpr double RIVER_SLOWPLAY_CHANCE = 0.25;

    //each prior slowplay this hand sharply cuts the chance of doing it again
    constexpr double SLOWPLAY_DECAY = 0.4;

    //whether to slowplay a raise-worthy hand instead of raising it this
    //decision, so value aggression isn't 100% deterministic - each
    //already-slowplayed street this hand cuts the chance further, so the
    //trap usually springs the very next street (call preflop, raise the
    //flop) and only occasionally drags out to the turn/river.
    //Re-raising into a raise should be a much rarer default than opening one -
    //flat-calling is the more natural response most of the time even with a
    //raise-worthy hand, both to disguise strength and because the equity
    //behind the raise-worthy bar still assumes random opponent cards, which
    //understates how much a raiser's real range has already narrowed; this
    //adds on top of (not instead of) the street-based chance above, and stacks
    //with each additional raise already faced this street
    constexpr double RERAISE_SLOWPLAY_BONUS_PER_RAISE = 0.25;
    constexpr double RERAISE_SLOWPLAY_CAP = 0.75;

    bool shouldSlowplay(CPUState& state, BettingStreet street, int raisesFacedThisStreet){
        double chance = street == BettingStreet::PreFlop ? PREFLOP_SLOWPLAY_CHANCE
                       : street == BettingStreet::Flop    ? FLOP_SLOWPLAY_CHANCE
                       : street == BettingStreet::Turn     ? TURN_SLOWPLAY_CHANCE
                                                            : RIVER_SLOWPLAY_CHANCE;

        if(raisesFacedThisStreet > 0){
            chance = std::min(
                chance + RERAISE_SLOWPLAY_BONUS_PER_RAISE * raisesFacedThisStreet,
                RERAISE_SLOWPLAY_CAP
            );
        }

        for(int i = 0; i < state.slowplaysThisHand; ++i){
            chance *= SLOWPLAY_DECAY;
        }

        if(randomChance() < chance){
            ++state.slowplaysThisHand;
            return true;
        }
        return false;
    }

    //a multiplier on raiseBar that grows with how many streets this player
    //has already bet/raised on this hand, so continuing to barrel needs more
    //and more edge instead of firing every street on autopilot once ahead;
    //the river is exempt since there's no future street left to hold back
    //for, so a hand that clears the bar should just bet it
    double barrelFatigue(int betsFiredThisHand, BettingStreet street){
        if(street == BettingStreet::River || betsFiredThisHand <= 0){
            return 1.0;
        }
        return 1.0 + 0.15 * betsFiredThisHand;
    }

    //a multiplier on raiseBar (and, by the caller, on the bluff-raise chance)
    //that grows steeply with how many raises the current street has already
    //seen - re-raising into a raise should take real extra strength, not
    //just the same edge-over-pot-odds bar used to continue against a
    //single, un-raised bet. The Monte Carlo equity behind that bar assumes
    //every opponent holds uniformly random cards, which stops being fair the
    //moment someone's actually raised - their real range is narrower and
    //stronger than random. Without this correction, two CPUs with similar
    //equity-vs-random would just keep raising each other back forever
    //instead of one settling into a flat call or fold. Returns 1.0
    //(neutral) for an opening raise rather than a re-raise, so normal
    //open-raising is unaffected
    double reraiseFatigue(int raisesFacedThisStreet){
        if(raisesFacedThisStreet <= 0){
            return 1.0;
        }
        return 1.0 + 0.5 * raisesFacedThisStreet;
    }

    //true if the board has at least one repeated rank (a pair, trips, or
    //quads out there) - a strong sign someone could easily have trips or a
    //full house, making it a much worse spot to represent strength with
    //nothing: a bluff both risks running into a real hand and is less
    //credible, since a paired board is exactly the kind of texture opponents
    //call down wider on
    bool boardIsPaired(const std::vector<Card>& board){
        std::array<int, 15> rankCount{};
        for(const Card& card : board){
            if(++rankCount[static_cast<int>(card.rank)] >= 2){
                return true;
            }
        }
        return false;
    }

    //true if the board has 3+ cards of one suit - a flush is live for anyone
    //holding two more of it
    bool boardHasFlushDraw(const std::vector<Card>& board){
        std::array<int, 4> suitCount{};

        for(const Card& card : board){
            ++suitCount[static_cast<int>(card.suit)];
        }

        for(int count : suitCount){
            if(count >= 3){
                return true;
            }
        }

        return false;
    }

    //true if the board has at least 3 distinct ranks (ace counted both high
    //and low) within some 5-consecutive-rank window - a straight is live for
    //the right two connecting cards
    bool boardHasStraightDraw(const std::vector<Card>& board){
        std::array<bool, 15> present{};

        for(const Card& card : board){
            present[static_cast<int>(card.rank)] = true;
        }
        present[1] = present[14];//ace can also play low

        for(int windowLow = 1; windowLow <= 10; ++windowLow){
            int count = 0;
            for(int rank = windowLow; rank < windowLow + 5; ++rank){
                if(present[rank]) ++count;
            }
            if(count >= 3){
                return true;
            }
        }

        return false;
    }

    //0 (dry), 1 (one draw type live), or 2 (both a flush draw and a
    //straight draw live - very wet)
    int boardWetness(const std::vector<Card>& board){
        int wetness = 0;

        if(boardHasFlushDraw(board)) ++wetness;
        if(boardHasStraightDraw(board)) ++wetness;

        return wetness;
    }

    //a dampening multiplier on bluff frequency based on board danger -
    //paired boards get bluffed much less; unchanged (1.0) preflop (empty
    //board) or on a safe/dry board
    double boardDangerDampening(const std::vector<Card>& board){
        if(!board.empty() && boardIsPaired(board)){
            return 0.3;
        }
        return 1.0;
    }

    //a dampening multiplier on bluff frequency based on how much of the
    //remaining stack a "strong tier" bluff bet (60%-90% of the pot, the same
    //sizing bluffs reuse to stay indistinguishable from value bets) would
    //use up. Shoving all-in as a bluff is the riskiest, least-foldable
    //version of one - no room left to fold to further aggression, and
    //getting called by literally any real hand ends it on the spot - so it
    //should be much rarer than a normal-sized bluff, unless the stack's
    //already shallow enough that a shove is close to unavoidable anyway
    double allInBluffDampening(const BasicCPUState& basicState){
        if(basicState.maxRaise <= 0 || basicState.potSize <= 0){
            return 1.0;
        }
        double stackToPot = static_cast<double>(basicState.maxRaise) / basicState.potSize;
        if(stackToPot < 0.9){
            return 0.35;
        }
        return 1.0;
    }

    //a dampening multiplier on bluff frequency based on calledFraction - a
    //bluff needs realistic fold equity, and that craters fast as more of the
    //table's already committed to continuing. Used by TightCPU/BalancedCPU's
    //flat bluff-frequency roll, which (unlike BlufferCPU::bluffChance())
    //otherwise has no caller-count awareness at all
    double manyCallersBluffDampening(const BasicCPUState& basicState){
        return std::clamp(1.0 - calledFraction(basicState) * 0.9, 0.1, 1.0);
    }

    //a multiplier on raiseBar/callBar (or divisor on a bluff-frequency roll)
    //for when this player's last bluff got called or raised over instead of
    //folded to - a real signal the story isn't being bought, so pushing more
    //chips in without a real hand needs real justification afterward, not
    //just "there's already a lot committed to the pot." Exempted when the
    //opponent driving this decision has a track record of bluffing a lot
    //themselves (only knowable for BalancedCPU, which tracks opponents -
    //Tight/BlufferCPU have no per-opponent history to check, so they never
    //get the exemption)
    double bluffNotWorkingPenalty(bool bluffCalledThisHand, bool opponentBluffsALot){
        if(!bluffCalledThisHand || opponentBluffsALot){
            return 1.0;
        }
        return 2.0;
    }

    //equity discounted for facing a shove from the last aggressor - the
    //Monte Carlo equity above assumes their hand is uniformly random, but a
    //real all-in is backed by actual chips and typically represents a
    //stronger range than that assumption credits (or, if it's a bluff,
    //still no worse than random). Discounting the equity used for the
    //calling decision keeps a hand that only edges out literal trash (bare
    //ace-high, say) from looking like a profitable call against a genuine shove
    double discountForAllIn(double equity, const BasicCPUState& basicState){
        if(basicState.lastAggressor != nullptr && basicState.lastAggressor->allIn()){
            return equity * 0.80;
        }
        return equity;
    }

    //equity discounted for facing a bet/raise - the same reasoning
    //discountForAllIn applies to a shove specifically, generalized to ANY
    //bet/raise, preflop or postflop: Monte Carlo equity assumes every
    //opponent holds a uniformly random hand, but a real opponent who chose
    //to bet/raise (rather than check/call) is already skewed toward a
    //stronger-than-random range. This baseline discount applies even on an
    //empty (preflop) or dry board, and deepens with each additional raise
    //the current street has already seen (a 3-bet+ range is tighter than a
    //single open/bet). Without this baseline, only literal shoves
    //(discountForAllIn) or dangerous board textures (below) corrected for a
    //stronger-than-random range at all, so an ordinary raise into an empty
    //preflop board or a dry flop was still valued at raw, uncorrected equity -
    //overstating margin enough to push marginal hands into raising/re-raising
    //instead of the flat call their real equity supports. A further,
    //board-texture-specific discount stacks on top when the board itself
    //specifically rewards trips/a full house/a flush/a straight (paired, or
    //wet enough for one to be live) - a real opponent betting into that
    //texture skews even further toward exactly those hands than the
    //baseline credits; paired is the more severe case (trips/a boat are
    //already made, not just drawing to), so it gets the bigger extra discount
    double discountForAggression(double equity, const std::vector<Card>& board, bool facingBet,
                                 int raisesFacedThisStreet){
        if(!facingBet){
            return equity;
        }

        double aggressionDiscount = std::clamp(
            0.97 - 0.03 * std::max(0, raisesFacedThisStreet - 1), 0.85, 0.97
        );

        double boardDiscount = 1.0;
        if(boardIsPaired(board)){
            boardDiscount = 0.90;
        }
        else if(boardWetness(board) >= 1){
            boardDiscount = 0.95;
        }

        return equity * aggressionDiscount * boardDiscount;
    }

    //a multiplier that raises the CALL bar as more players are live in the
    //hand. situationalDifficulty()'s callerFactor already loosens the call
    //bar as callers increase (implied-odds reasoning: a big pot down the
    //road rewards hands that improve to something huge) - fine for a
    //drawing hand, but a "decent," not great, MADE hand needs MORE caution
    //as more opponents are live, not less: more live hands means more
    //chances someone else has already connected too, or improves past a
    //thin made hand by the river. Neutral (1.0) heads-up (a single
    //opponent), since that's exactly the case callerFactor's reasoning and
    //the Minimum Defense Frequency floor below are built for.
    //0.12/opponent capped at 2.0 (the original AdvancedCPU-only tuning) was
    //fine as a single mechanism, but once shared across all four personalities
    //alongside situationalDifficulty()'s own already-present position/stack-
    //depth factors and a separately-raised callMargin baseline, the combined
    //multiplicative stack proved far stronger than any one piece was tuned in
    //isolation for - a standalone diagnostic showed a fresh (non-adapted)
    //TightCPU folding K-J offsuit (60% equity) to a MINIMUM raise 99% of the
    //time at an 8-handed table, purely from this compounding. Cut to
    //0.05/opponent, capped at 1.4, so this stays a modest nudge on top of the
    //other factors rather than another dominant multiplier
    double multiwayCallCaution(int playersInHand){
        int liveOpponents = std::max(0, playersInHand - 1);
        return std::clamp(1.0 + 0.05 * std::max(0, liveOpponents - 1), 1.0, 1.4);
    }

    //number of Monte Carlo trials behind each AdvancedCPU::ActionRecord's
    //equityAtAction - far cheaper than CPU_SIMULATIONS since this runs once
    //per real action OBSERVED (i.e. for every player's every action, not just
    //once per live decision) and only needs to be accurate in aggregate over
    //many logged samples, not individually precise
    inline constexpr int ADVANCED_HISTORY_SIMULATIONS = 3000;

    //equity a raise-like action needs to clear to count as "for value" rather
    //than a bluff when AdvancedCPU classifies logged history - higher than
    //OPENING_BET_EQUITY_FLOOR since a raise/reraise is judged against genuine
    //value strength, not just "worth entering the pot"
    constexpr double ADVANCED_VALUE_EQUITY_BAR = 0.55;

    //number of DISTINCT hands (not some narrower/sparser action type - see
    //AdvancedCPU::handsSeenForPlayer) observed of one opponent before
    //AdvancedCPU's exploit read on them is trusted at full strength - below
    //this, buildExploitRead() blends its multipliers toward neutral in
    //proportion to how few hands have actually been seen of them. There
    //isn't much to go on the first couple of hands, but by this many hands
    //in, their behavior should start meaningfully shaping the read
    inline constexpr int ADVANCED_FULL_CONFIDENCE_HANDS = 10;

    //how close (in bet-to-pot ratio) a historical raise must be to the bet
    //actually being sized THIS decision to count as "similarly sized" for
    //sizingTellBluffRate() - wide enough to gather samples (real opponents
    //rarely repeat an exact sizing) but narrow enough to still mean "this
    //general sizing" rather than blending in totally different bet sizes
    constexpr double ADVANCED_SIZING_TELL_BAND = 0.25;

    //minimum number of similarly-sized historical raises needed before
    //sizingTellBluffRate() trusts them enough to return a signal instead of -1
    inline constexpr int ADVANCED_SIZING_TELL_MIN_SAMPLES = 4;

    //width (in margin units) of the randomized zone mixedStrategyProbability()
    //blends through around its bar - roughly, decisions within about 2-3x this
    //width of the bar get materially randomized; well beyond that the decision
    //is effectively deterministic, so clearly +EV/-EV plays aren't randomized away.
    //Used for the raise-margin and call-margin decisions, where margin/bar are
    //both small (raiseMargin/callMargin are typically 0.05-0.35), so this width
    //is proportionate to them
    constexpr double ADVANCED_MIX_WIDTH = 0.05;

    //separate, much narrower width for the preflop starting-hand screen
    //specifically - that decision compares raw heads-up EQUITY (roughly a
    //15%-85% range across hands) against startingHandThreshold, a totally
    //different scale than the raise/call margins above. Reusing
    //ADVANCED_MIX_WIDTH there made a hand only a few points above the
    //threshold - e.g. A8s (~60% equity) against a jittered/positionally
    //undiscounted threshold near 0.56 - get randomized as if it were a
    //genuine coin flip, when it's actually a comfortably profitable open;
    //this narrower width keeps the mixing meaningful only for hands that are
    //ACTUALLY close to the threshold, not merely somewhat above/below it
    constexpr double ADVANCED_PREFLOP_MIX_WIDTH = 0.02;
}


int Player::bigBlind = 0;

Player::Player(const std::string &nameIn, int chipsIn, int playerIDin)
              : name(nameIn), totalChips(chipsIn), playerID(playerIDin){}

void Player::addChips(int numChips){
    totalChips += numChips;
}

void Player::addToCurrentBet(int numChips){
    assert(numChips >= 0);
    assert(numChips <= totalChips);

    currentBet += numChips;
    totalChipsInHand += numChips;
    totalChips -= numChips;
}

int Player::getNumChips() const{
    return totalChips;
}

int Player::getCurrentBet() const{
    return currentBet;
}

int Player::getTotalChipsInHand() const{
    return totalChipsInHand;
}

int Player::getID() const{
    return playerID;
}

void Player::addCard(Card &c, int i){
    hand[i] = c;
}

const std::array<Card, 2>& Player::getHand() const{
    return hand;
}

bool Player::folded() const{
    return !inHand;
}

bool Player::allIn() const{
    return totalChips == 0;
}

const std::string& Player::getName() const{
    return name;
}

void Player::fold(){
    inHand = false;
}

void Player::setActedSinceLastRaise(bool val){
    actedSinceLastRaise = val;
}

void Player::resetForNewHand(){
    inHand = true;
    totalChipsInHand = 0;
    currentBet = 0;
    actedSinceLastRaise = false;
}

void Player::resetForNewBettingRound(){
    currentBet = 0;
    actedSinceLastRaise = false;
}

bool Player::hasActedSinceLastRaise() const{
    return actedSinceLastRaise;
}

//no-op by default - only opponent-modeling CPU types override this
void Player::observeAction(const Player&, const PlayerAction&, const BasicCPUState&){}

//no-op by default - only opponent-modeling CPU types override this
void Player::observeShowdown(const Player&, const std::vector<Card>&, const HandValue&, bool){}

std::unique_ptr<Player> createCPUPlayer(PlayerType type, const std::string &name,
                                        int buyIn, int id){
    switch(type){
        case PlayerType::TightCPU:
            return std::make_unique<TightCPU>(name, buyIn, id);

        case PlayerType::BlufferCPU:
            return std::make_unique<BlufferCPU>(name, buyIn, id);

        case PlayerType::BalancedCPU:
            return std::make_unique<BalancedCPU>(name, buyIn, id);

        case PlayerType::AdvancedCPU:
            return std::make_unique<AdvancedCPU>(name, buyIn, id);

        default:
            throw std::invalid_argument("Invalid CPU player type");
    }
}



CPU::CPU(const std::string &nameIn, int chipsIn, int playerIDin)
    : Player(nameIn, chipsIn, playerIDin){}

void CPU::resetForNewHand(){
    Player::resetForNewHand();
    state.slowplaysThisHand = 0;
    state.betsFiredThisHand = 0;
    state.raisesFacedThisStreet = 0;
    state.lastSeenBoardSizeForRaiseTracking = -1;
    state.lastRaiseWasBluff = false;
    state.bluffCalledThisHand = false;
}

void CPU::observeAction(const Player& actor, const PlayerAction& action,
                         const BasicCPUState& stateBeforeAction){
    int boardSize = static_cast<int>(stateBeforeAction.board.size());
    if(boardSize != state.lastSeenBoardSizeForRaiseTracking){
        state.raisesFacedThisStreet = 0;
        state.lastSeenBoardSizeForRaiseTracking = boardSize;
    }

    //an all-in that increases the bet is raise-like; one that merely covers
    //(or falls short of) the existing bet is not - mirrors applyAction's own
    //newPlayerBet > currentHighestBet check exactly
    bool raiseLike = action.action == Action::Raise ||
        (action.action == Action::AllIn && actor.getCurrentBet() > stateBeforeAction.currentHighestBet);

    if(raiseLike){
        ++state.raisesFacedThisStreet;
    }

    //if this player's last raise was a bluff and someone else just responded
    //to it, that response resolves whether the bluff worked - a fold means it
    //did, anything else (call, raise, all-in) means the story wasn't bought
    if(&actor != this && stateBeforeAction.lastAggressor == this && state.lastRaiseWasBluff){
        if(action.action != Action::Fold){
            state.bluffCalledThisHand = true;
        }
        state.lastRaiseWasBluff = false;
    }
}

TightCPU::TightCPU(const std::string &nameIn, int chipsIn, int playerIDin)
    : CPU(nameIn, chipsIn, playerIDin){
    //heads-up-equivalent equity floor - roughly TT+/AQ+ and better, a tight opening range
    state.parameters.startingHandThreshold = 0.58;
    //0.08 -> 0.11: too many hands that clear the starting-hand screen were
    //still calling a real raise almost automatically, since raw Monte Carlo
    //equity (vs a uniformly random hand) doesn't reflect how much stronger
    //a real raiser's range is than random - see discountForAggression()
    state.parameters.callMargin = 0.11;
    state.parameters.raiseMargin = 0.18;
    state.parameters.bluffFrequency = 0.02;
    state.parameters.riskTolerance = 0.08;
}

PlayerAction TightCPU::act(const BasicCPUState& basicState){
    Equity equityCalc(hand, basicState.board, CPU_SIMULATIONS, basicState.playersInHand - 1);

    EquityResult result = equityCalc.calculate();

    bool canRaise = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                              Action::Raise) != basicState.availableActions.end();
    bool canCall = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                             Action::Call) != basicState.availableActions.end();
    bool canCheck = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                              Action::Check) != basicState.availableActions.end();
    bool canAllIn = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                              Action::AllIn) != basicState.availableActions.end();

    int amountToCall = basicState.currentHighestBet - currentBet;

    //preflop hands are screened by starting-hand strength before pot odds are even considered
    bool preflop = basicState.board.empty();

    //whether this decision is genuinely responding to a raise/bet, for
    //discountForAggression() below - facing only the big blind preflop
    //(amountToCall > 0 purely from the cost of entering the pot, nobody
    //having actually raised) does NOT count, even though amountToCall is
    //still positive there; blinds are posted directly in Game::preFlop(),
    //never through the act()/observeAction() loop, so state.raisesFacedThisStreet
    //stays 0 until a real raise happens - same "has anyone actually raised"
    //test openPriceLooseness() already applies to the starting-hand
    //threshold. Postflop, currentHighestBet resets to 0 each street, so any
    //positive amountToCall there is inherently a real bet - no blind-
    //equivalent noise to filter out
    bool facingRealAggression = preflop
        ? (amountToCall > 0 && basicState.currentHighestBet > Player::bigBlind)
        : (amountToCall > 0);
    BettingStreet street = streetFromBoard(basicState.board);
    if(preflop && amountToCall > 0 && preflopHandStrength(hand) < state.parameters.startingHandThreshold * positionalLooseness(basicState.position) * openPriceLooseness(basicState.currentHighestBet)){
        if(canRaise && randomChance() < state.parameters.bluffFrequency / reraiseFatigue(state.raisesFacedThisStreet) * boardDangerDampening(basicState.board) * allInBluffDampening(basicState) * manyCallersBluffDampening(basicState) / bluffNotWorkingPenalty(state.bluffCalledThisHand, false)){
            int raiseAmount = std::clamp(
                valueBetSize(0.75, basicState.potSize),
                basicState.minRaise,
                basicState.maxRaise
            );
            ++state.betsFiredThisHand;
            state.lastRaiseWasBluff = true;
            return {Action::Raise, raiseAmount};
        }
        return {Action::Fold, 0};
    }

    //past the starting-hand screen (or postflop), continue based on equity versus pot odds -
    //leading out (nothing to call) uses OPENING_BET_EQUITY_FLOOR in place of real pot odds,
    //since "any equity above zero" is not a real bar for betting when nobody's making you
    double potOdds = amountToCall > 0
        ? static_cast<double>(amountToCall) / (basicState.potSize + amountToCall)
        : OPENING_BET_EQUITY_FLOOR;
    //preflop specifically, the RAISE decision needs a more realistic
    //opponent count than "literally everyone still dealt into the hand,"
    //for the exact same reason CALLING (below) uses heads-up equity instead
    //of raw multiway - see preflopRaiseEquity()'s comment
    double raiseEquityBasis = discountForAggression(
        preflop ? preflopRaiseEquity(hand, basicState) : result.equity,
        basicState.board, facingRealAggression, state.raisesFacedThisStreet
    );
    double margin = raiseEquityBasis - potOdds;

    //preflop specifically, CALLING is judged against heads-up hand strength
    //rather than the raw multiway Monte Carlo equity - multiway equity
    //against every player still dealt in craters fast as table size grows
    //(even pocket aces are only ~49% at 6-max), which assumes a worst-case
    //"everyone stays to the end" that isn't realistic preflop, where most of
    //the field folds before it matters. A hand that already cleared the
    //heads-up screen above shouldn't be re-punished by a second, much
    //harsher equity bar right after it - raising above uses
    //preflopRaiseEquity() instead of the raw multiway margin for the same
    //reason, just scaled by however many opponents have actually shown
    //interest so far rather than assuming literally everyone stays in
    double callEquityBasis = discountForAggression(
        discountForAllIn(preflop ? preflopHandStrength(hand) : result.equity, basicState),
        basicState.board, facingRealAggression, state.raisesFacedThisStreet
    );
    double callDecisionMargin = callEquityBasis - potOdds;

    //even at a great price, calling preflop still needs some absolute hand
    //quality - pot odds alone (especially the blinds', who already have
    //money sunk in and so see a cheaper price than anyone else) shouldn't be
    //enough to justify continuing with any two cards; postflop, real
    //board-connected equity already encodes this, so the floor only applies preflop
    bool clearsCallFloor = !preflop || callEquityBasis >= state.parameters.startingHandThreshold * 0.85;

    //real seat position (not just the playersYetToAct proxy situationalDifficulty
    //uses) - neutral preflop (already applied to the starting-hand threshold
    //there; applying it again here would double-count), late position gets a
    //discount postflop, matching standard "play tighter early, looser late"
    //position play
    double positionMult = preflop ? 1.0 : positionalLooseness(basicState.position);

    //the bar to clear scales with position, live callers, the aggressor's stack, pot depth in big
    //blinds, and (for raising) how many streets this hand this CPU has already bet/raised on
    double raiseBar = state.parameters.raiseMargin
        * situationalDifficulty(basicState, this, state.parameters.riskTolerance, true)
        * barrelFatigue(state.betsFiredThisHand, street)
        * reraiseFatigue(state.raisesFacedThisStreet)
        * positionMult
        * bluffNotWorkingPenalty(state.bluffCalledThisHand, false);
    double callBar = state.parameters.callMargin
        * situationalDifficulty(basicState, this, state.parameters.riskTolerance, false)
        * positionMult
        * (facingRealAggression ? multiwayCallCaution(basicState.playersInHand) : 1.0)
        * bluffNotWorkingPenalty(state.bluffCalledThisHand, false);

    if(canRaise && margin >= raiseBar && !shouldSlowplay(state, street, state.raisesFacedThisStreet)){
        int raiseAmount = std::clamp(
            valueBetSize(raiseEquityBasis, basicState.potSize),
            basicState.minRaise,
            basicState.maxRaise
        );
        ++state.betsFiredThisHand;
        state.lastRaiseWasBluff = false;
        return {Action::Raise, raiseAmount};
    }

    if(canRaise && randomChance() < state.parameters.bluffFrequency / reraiseFatigue(state.raisesFacedThisStreet) * boardDangerDampening(basicState.board) * allInBluffDampening(basicState) * manyCallersBluffDampening(basicState) / bluffNotWorkingPenalty(state.bluffCalledThisHand, false)){
        int raiseAmount = std::clamp(
            valueBetSize(0.75, basicState.potSize),
            basicState.minRaise,
            basicState.maxRaise
        );
        ++state.betsFiredThisHand;
        state.lastRaiseWasBluff = true;
        return {Action::Raise, raiseAmount};
    }

    if(canCall && callDecisionMargin >= callBar && clearsCallFloor){
        return {Action::Call, 0};
    }

    //short-stacked and facing a bet larger than the remaining stack - Call
    //isn't legal here, but calling all-in is the same decision mechanically,
    //just capped by the stack, so it uses the same equity-vs-pot-odds bar
    if(!canCall && canAllIn && amountToCall > 0 && callDecisionMargin >= callBar && clearsCallFloor){
        return {Action::AllIn, 0};
    }

    if(canCheck){
        return {Action::Check, 0};
    }

    return {Action::Fold, 0};
}

BlufferCPU::BlufferCPU(const std::string &nameIn, int chipsIn, int playerIDin)
    : CPU(nameIn, chipsIn, playerIDin){
    //heads-up-equivalent equity floor - plays a wide range, consistent with a loose-aggressive style
    state.parameters.startingHandThreshold = 0.44;
    //0.04 -> 0.07: this was low enough that almost anything clearing the
    //already-wide starting-hand screen also cleared the call bar facing a
    //real raise (a standalone diagnostic showed ~98-99% call, ~0-1% fold,
    //with a mediocre hand like Q5s) - "loose" should still mean real folds
    //sometimes, not fold-almost-never; kept below TightCPU's 0.11 so Bluffer
    //still calls markedly wider
    state.parameters.callMargin = 0.07;
    state.parameters.raiseMargin = 0.10;
    state.parameters.bluffFrequency = 0.10;
    state.parameters.riskTolerance = 0.18;
}

void BlufferCPU::resetForNewHand(){
    CPU::resetForNewHand();

    if(state.recentBluffHands > 0){
        --state.recentBluffHands;
    }
}

double BlufferCPU::bluffChance(const BasicCPUState& basicState) const{
    //each live caller already showed they're willing to stick around, so fold equity drops fast
    double callerFactor = 1.0 / (1 + basicState.callersThisStreet);

    //bluffs need every remaining opponent to fold, so credibility fades the more players are in the hand
    double playersFactor = 1.0 / (basicState.playersInHand - 1);

    //fewer players still needing to act means less risk of getting raised off the bluff before it closes
    double positionFactor = std::clamp(1.0 - 0.1 * basicState.playersYetToAct, 0.4, 1.0);

    //how likely lastAggressor actually is to fold to your bluff-raise,
    //based on how much stack they have left relative to what's already in the pot.
    double stackFactor = std::clamp(aggressorStackToPot(basicState, this), 0.25, 1.5);

    //cools off for a couple of hands after firing a bluff, rather than being
    //just as likely to bluff again immediately - fades back to 1.0 (normal)
    //once recentBluffHands has decayed to 0 in resetForNewHand()
    double cooldownFactor = 1.0 - 0.5 * state.recentBluffHands / BLUFF_COOLDOWN_HANDS;

    double chance = state.parameters.bluffFrequency
                   * callerFactor
                   * playersFactor
                   * positionFactor
                   * stackFactor
                   * (1.0 + state.parameters.riskTolerance)
                   * cooldownFactor;

    return std::clamp(chance, 0.0, 0.6);
}

PlayerAction BlufferCPU::act(const BasicCPUState& basicState){
    Equity equityCalc(hand, basicState.board, CPU_SIMULATIONS, basicState.playersInHand - 1);

    EquityResult result = equityCalc.calculate();

    bool canRaise = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                              Action::Raise) != basicState.availableActions.end();
    bool canCall = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                             Action::Call) != basicState.availableActions.end();
    bool canCheck = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                              Action::Check) != basicState.availableActions.end();
    bool canAllIn = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                              Action::AllIn) != basicState.availableActions.end();

    int amountToCall = basicState.currentHighestBet - currentBet;

    //preflop hands are screened by starting-hand strength before pot odds are even considered
    bool preflop = basicState.board.empty();

    //whether this decision is genuinely responding to a raise/bet, for
    //discountForAggression() below - facing only the big blind preflop
    //(amountToCall > 0 purely from the cost of entering the pot, nobody
    //having actually raised) does NOT count, even though amountToCall is
    //still positive there; blinds are posted directly in Game::preFlop(),
    //never through the act()/observeAction() loop, so state.raisesFacedThisStreet
    //stays 0 until a real raise happens - same "has anyone actually raised"
    //test openPriceLooseness() already applies to the starting-hand
    //threshold. Postflop, currentHighestBet resets to 0 each street, so any
    //positive amountToCall there is inherently a real bet - no blind-
    //equivalent noise to filter out
    bool facingRealAggression = preflop
        ? (amountToCall > 0 && basicState.currentHighestBet > Player::bigBlind)
        : (amountToCall > 0);
    BettingStreet street = streetFromBoard(basicState.board);
    if(preflop && amountToCall > 0 && preflopHandStrength(hand) < state.parameters.startingHandThreshold * positionalLooseness(basicState.position) * openPriceLooseness(basicState.currentHighestBet)){
        //a bluff is sized the same as a strong value raise so the two stay indistinguishable
        if(canRaise && randomChance() < bluffChance(basicState) / reraiseFatigue(state.raisesFacedThisStreet) * boardDangerDampening(basicState.board) * allInBluffDampening(basicState) / bluffNotWorkingPenalty(state.bluffCalledThisHand, false)){
            int raiseAmount = std::clamp(
                valueBetSize(0.75, basicState.potSize),
                basicState.minRaise,
                basicState.maxRaise
            );
            state.recentBluffHands = BLUFF_COOLDOWN_HANDS;
            ++state.betsFiredThisHand;
            state.lastRaiseWasBluff = true;
            return {Action::Raise, raiseAmount};
        }
        return {Action::Fold, 0};
    }

    //past the starting-hand screen (or postflop), continue based on equity versus pot odds -
    //leading out (nothing to call) uses OPENING_BET_EQUITY_FLOOR in place of real pot odds,
    //since "any equity above zero" is not a real bar for betting when nobody's making you
    double potOdds = amountToCall > 0
        ? static_cast<double>(amountToCall) / (basicState.potSize + amountToCall)
        : OPENING_BET_EQUITY_FLOOR;
    //preflop specifically, the RAISE decision needs a more realistic
    //opponent count than "literally everyone still dealt into the hand,"
    //for the exact same reason CALLING (below) uses heads-up equity instead
    //of raw multiway - see preflopRaiseEquity()'s comment
    double raiseEquityBasis = discountForAggression(
        preflop ? preflopRaiseEquity(hand, basicState) : result.equity,
        basicState.board, facingRealAggression, state.raisesFacedThisStreet
    );
    double margin = raiseEquityBasis - potOdds;

    //preflop specifically, CALLING is judged against heads-up hand strength
    //rather than the raw multiway Monte Carlo equity - multiway equity
    //against every player still dealt in craters fast as table size grows
    //(even pocket aces are only ~49% at 6-max), which assumes a worst-case
    //"everyone stays to the end" that isn't realistic preflop, where most of
    //the field folds before it matters. A hand that already cleared the
    //heads-up screen above shouldn't be re-punished by a second, much
    //harsher equity bar right after it - raising above uses
    //preflopRaiseEquity() instead of the raw multiway margin for the same
    //reason, just scaled by however many opponents have actually shown
    //interest so far rather than assuming literally everyone stays in
    double callEquityBasis = discountForAggression(
        discountForAllIn(preflop ? preflopHandStrength(hand) : result.equity, basicState),
        basicState.board, facingRealAggression, state.raisesFacedThisStreet
    );
    double callDecisionMargin = callEquityBasis - potOdds;

    //even at a great price, calling preflop still needs some absolute hand
    //quality - pot odds alone (especially the blinds', who already have
    //money sunk in and so see a cheaper price than anyone else) shouldn't be
    //enough to justify continuing with any two cards; postflop, real
    //board-connected equity already encodes this, so the floor only applies preflop
    bool clearsCallFloor = !preflop || callEquityBasis >= state.parameters.startingHandThreshold * 0.85;

    //real seat position (not just the playersYetToAct proxy situationalDifficulty
    //uses) - neutral preflop (already applied to the starting-hand threshold
    //there; applying it again here would double-count), late position gets a
    //discount postflop, matching standard "play tighter early, looser late"
    //position play
    double positionMult = preflop ? 1.0 : positionalLooseness(basicState.position);

    //the bar to clear scales with position, live callers, the aggressor's stack, pot depth in big
    //blinds, and (for raising) how many streets this hand this CPU has already bet/raised on
    double raiseBar = state.parameters.raiseMargin
        * situationalDifficulty(basicState, this, state.parameters.riskTolerance, true)
        * barrelFatigue(state.betsFiredThisHand, street)
        * reraiseFatigue(state.raisesFacedThisStreet)
        * positionMult
        * bluffNotWorkingPenalty(state.bluffCalledThisHand, false);
    double callBar = state.parameters.callMargin
        * situationalDifficulty(basicState, this, state.parameters.riskTolerance, false)
        * positionMult
        * (facingRealAggression ? multiwayCallCaution(basicState.playersInHand) : 1.0)
        * bluffNotWorkingPenalty(state.bluffCalledThisHand, false);

    if(canRaise && margin >= raiseBar && !shouldSlowplay(state, street, state.raisesFacedThisStreet)){
        int raiseAmount = std::clamp(
            valueBetSize(raiseEquityBasis, basicState.potSize),
            basicState.minRaise,
            basicState.maxRaise
        );
        ++state.betsFiredThisHand;
        state.lastRaiseWasBluff = false;
        return {Action::Raise, raiseAmount};
    }

    if(canRaise && randomChance() < bluffChance(basicState) / reraiseFatigue(state.raisesFacedThisStreet) * boardDangerDampening(basicState.board) * allInBluffDampening(basicState) / bluffNotWorkingPenalty(state.bluffCalledThisHand, false)){
        int raiseAmount = std::clamp(
            valueBetSize(0.75, basicState.potSize),
            basicState.minRaise,
            basicState.maxRaise
        );
        state.recentBluffHands = BLUFF_COOLDOWN_HANDS;
        ++state.betsFiredThisHand;
        state.lastRaiseWasBluff = true;
        return {Action::Raise, raiseAmount};
    }

    if(canCall && callDecisionMargin >= callBar && clearsCallFloor){
        return {Action::Call, 0};
    }

    //short-stacked and facing a bet larger than the remaining stack - Call
    //isn't legal here, but calling all-in is the same decision mechanically,
    //just capped by the stack, so it uses the same equity-vs-pot-odds bar
    if(!canCall && canAllIn && amountToCall > 0 && callDecisionMargin >= callBar && clearsCallFloor){
        return {Action::AllIn, 0};
    }

    if(canCheck){
        return {Action::Check, 0};
    }

    return {Action::Fold, 0};
}

//tracking data is shared across every BalancedCPU instance (static) rather
//than per-instance, since every instance observes the exact same public
//action stream - there's no benefit to each maintaining an independent copy
int BalancedCPU::handsObserved = 0;
std::vector<OpponentStats> BalancedCPU::opponentStats;
std::vector<bool> BalancedCPU::vpipCountedThisHand;
std::vector<int> BalancedCPU::handsSeenByPlayer;
std::vector<bool> BalancedCPU::seenThisHand;
int BalancedCPU::lastAggressorIdThisHand = -1;
BalancedCPU::StreetContext BalancedCPU::streetContext{};

//number of hands of history on one specific opponent needed before their
//tracked tendencies are trusted at full strength - below this, readOpponent()
//blends its multipliers toward neutral (1.0) in proportion to how few hands
//have actually been seen, so a brand-new opponent (or the first few hands of
//a session) gets judged on equity/position/pot-odds alone, same as the other
//CPU types, rather than off a false read built on too little data
inline constexpr int OPPONENT_FULL_CONFIDENCE_HANDS = 20;

BalancedCPU::BalancedCPU(const std::string &nameIn, int chipsIn, int playerIDin)
    : CPU(nameIn, chipsIn, playerIDin){
    auto jitter = [](double base){ return base * (0.85 + randomChance() * 0.30); };

    //heads-up-equivalent equity floor, between TightCPU's (0.58) and BlufferCPU's (0.44)
    state.parameters.startingHandThreshold = jitter(0.51);
    //0.06 -> 0.085 baseline, same reasoning as TightCPU/BlufferCPU above -
    //too low to ever meaningfully fold once a hand clears the preflop screen
    state.parameters.callMargin = jitter(0.085);
    state.parameters.raiseMargin = jitter(0.14);
    state.parameters.bluffFrequency = jitter(0.06);
    state.parameters.riskTolerance = jitter(0.13);
}

PlayerAction BalancedCPU::act(const BasicCPUState& basicState){
    Equity equityCalc(hand, basicState.board, CPU_SIMULATIONS, basicState.playersInHand - 1);
    EquityResult result = equityCalc.calculate();

    bool canRaise = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                              Action::Raise) != basicState.availableActions.end();
    bool canCall = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                             Action::Call) != basicState.availableActions.end();
    bool canCheck = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                              Action::Check) != basicState.availableActions.end();
    bool canAllIn = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                              Action::AllIn) != basicState.availableActions.end();

    int amountToCall = basicState.currentHighestBet - currentBet;

    //preflop hands are screened by starting-hand strength before pot odds are even considered
    bool preflop = basicState.board.empty();

    //whether this decision is genuinely responding to a raise/bet, for
    //discountForAggression() below - facing only the big blind preflop
    //(amountToCall > 0 purely from the cost of entering the pot, nobody
    //having actually raised) does NOT count, even though amountToCall is
    //still positive there; blinds are posted directly in Game::preFlop(),
    //never through the act()/observeAction() loop, so state.raisesFacedThisStreet
    //stays 0 until a real raise happens - same "has anyone actually raised"
    //test openPriceLooseness() already applies to the starting-hand
    //threshold. Postflop, currentHighestBet resets to 0 each street, so any
    //positive amountToCall there is inherently a real bet - no blind-
    //equivalent noise to filter out
    bool facingRealAggression = preflop
        ? (amountToCall > 0 && basicState.currentHighestBet > Player::bigBlind)
        : (amountToCall > 0);
    BettingStreet street = streetFromBoard(basicState.board);

    //the opponent this decision is really "about": whoever bet/raised to create
    //this decision, or (if nobody has bet yet this street - opening or c-bet
    //spots) whoever was last aggressive anywhere earlier in the hand; -1 (no
    //read, i.e. every OpponentRead multiplier stays neutral) if neither applies
    int opponentId = -1;
    if(basicState.lastAggressor != nullptr && basicState.lastAggressor != this){
        opponentId = basicState.lastAggressor->getID();
    }
    else if(lastAggressorIdThisHand != -1 && lastAggressorIdThisHand != getID()){
        opponentId = lastAggressorIdThisHand;
    }
    OpponentRead read = readOpponent(opponentId, preflop);

    //whether the opponent driving this decision has shown down more bluffs
    //than value hands when they were the aggressor - a real (if noisy, since
    //showdowns are rare) signal that their own aggression might be more
    //bluff than it looks, worth weighing against one of our bluffs that just
    //got called or raised over instead of automatically giving up on the hand
    bool opponentBluffsALot = false;
    if(opponentId >= 0 && static_cast<std::size_t>(opponentId) < opponentStats.size()){
        const OpponentStats& oppStats = opponentStats[static_cast<std::size_t>(opponentId)];
        int shows = oppStats.revealedBluffs + oppStats.revealedValueBets;
        opponentBluffsALot = shows >= 2 && oppStats.revealedBluffs > oppStats.revealedValueBets;
    }

    if(preflop && amountToCall > 0 && preflopHandStrength(hand) < state.parameters.startingHandThreshold * positionalLooseness(basicState.position) * openPriceLooseness(basicState.currentHighestBet)){
        if(canRaise && randomChance() < std::clamp(state.parameters.bluffFrequency * read.bluffMult / reraiseFatigue(state.raisesFacedThisStreet), 0.0, 0.6) * boardDangerDampening(basicState.board) * allInBluffDampening(basicState) * manyCallersBluffDampening(basicState) / bluffNotWorkingPenalty(state.bluffCalledThisHand, opponentBluffsALot)){
            int raiseAmount = std::clamp(
                valueBetSize(0.75, basicState.potSize),
                basicState.minRaise,
                basicState.maxRaise
            );
            playedPreflopThisHand = true;
            firedBluffThisHand = true;
            ++state.betsFiredThisHand;
            state.lastRaiseWasBluff = true;
            return {Action::Raise, raiseAmount};
        }
        foldedThisHand = true;
        return {Action::Fold, 0};
    }

    //past the starting-hand screen (or postflop), continue based on equity versus pot odds -
    //leading out (nothing to call) uses OPENING_BET_EQUITY_FLOOR in place of real pot odds,
    //since "any equity above zero" is not a real bar for betting when nobody's making you
    double potOdds = amountToCall > 0
        ? static_cast<double>(amountToCall) / (basicState.potSize + amountToCall)
        : OPENING_BET_EQUITY_FLOOR;
    //preflop specifically, the RAISE decision needs a more realistic
    //opponent count than "literally everyone still dealt into the hand,"
    //for the exact same reason CALLING (below) uses heads-up equity instead
    //of raw multiway - see preflopRaiseEquity()'s comment
    double raiseEquityBasis = discountForAggression(
        preflop ? preflopRaiseEquity(hand, basicState) : result.equity,
        basicState.board, facingRealAggression, state.raisesFacedThisStreet
    );
    double margin = raiseEquityBasis - potOdds;

    //preflop specifically, CALLING is judged against heads-up hand strength
    //rather than the raw multiway Monte Carlo equity - multiway equity
    //against every player still dealt in craters fast as table size grows
    //(even pocket aces are only ~49% at 6-max), which assumes a worst-case
    //"everyone stays to the end" that isn't realistic preflop, where most of
    //the field folds before it matters. A hand that already cleared the
    //heads-up screen above shouldn't be re-punished by a second, much
    //harsher equity bar right after it - raising above uses
    //preflopRaiseEquity() instead of the raw multiway margin for the same
    //reason, just scaled by however many opponents have actually shown
    //interest so far rather than assuming literally everyone stays in
    double callEquityBasis = discountForAggression(
        discountForAllIn(preflop ? preflopHandStrength(hand) : result.equity, basicState),
        basicState.board, facingRealAggression, state.raisesFacedThisStreet
    );
    double callDecisionMargin = callEquityBasis - potOdds;

    //even at a great price, calling preflop still needs some absolute hand
    //quality - pot odds alone (especially the blinds', who already have
    //money sunk in and so see a cheaper price than anyone else) shouldn't be
    //enough to justify continuing with any two cards; postflop, real
    //board-connected equity already encodes this, so the floor only applies preflop
    bool clearsCallFloor = !preflop || callEquityBasis >= state.parameters.startingHandThreshold * 0.85;

    //real seat position (not just the playersYetToAct proxy situationalDifficulty
    //uses) - neutral preflop (already applied to the starting-hand threshold
    //there; applying it again here would double-count), late position gets a
    //discount postflop, matching standard "play tighter early, looser late"
    //position play
    double positionMult = preflop ? 1.0 : positionalLooseness(basicState.position);

    //the bar to clear scales with position, live callers, the aggressor's stack, pot commitment,
    //how many streets this hand this CPU has already bet/raised on, and (unlike TightCPU/BlufferCPU)
    //a read on the specific opponent driving this decision - a credible, rarely-aggressive opponent
    //raises the bar to continue/raise over them; a habitually aggressive or calling-station one lowers it
    double raiseBar = state.parameters.raiseMargin
        * situationalDifficulty(basicState, this, state.parameters.riskTolerance, true)
        * barrelFatigue(state.betsFiredThisHand, street)
        * reraiseFatigue(state.raisesFacedThisStreet)
        * read.raiseBarMult
        * positionMult
        * bluffNotWorkingPenalty(state.bluffCalledThisHand, opponentBluffsALot);
    double callBar = state.parameters.callMargin
        * situationalDifficulty(basicState, this, state.parameters.riskTolerance, false)
        * read.callBarMult
        * positionMult
        * (facingRealAggression ? multiwayCallCaution(basicState.playersInHand) : 1.0)
        * bluffNotWorkingPenalty(state.bluffCalledThisHand, opponentBluffsALot);

    if(canRaise && margin >= raiseBar && !shouldSlowplay(state, street, state.raisesFacedThisStreet)){
        int raiseAmount = std::clamp(
            valueBetSize(raiseEquityBasis, basicState.potSize),
            basicState.minRaise,
            basicState.maxRaise
        );
        if(preflop) playedPreflopThisHand = true;
        firedValueRaiseThisHand = true;
        ++state.betsFiredThisHand;
        state.lastRaiseWasBluff = false;
        return {Action::Raise, raiseAmount};
    }

    if(canRaise && randomChance() < std::clamp(state.parameters.bluffFrequency * read.bluffMult / reraiseFatigue(state.raisesFacedThisStreet), 0.0, 0.6) * boardDangerDampening(basicState.board) * allInBluffDampening(basicState) * manyCallersBluffDampening(basicState) / bluffNotWorkingPenalty(state.bluffCalledThisHand, opponentBluffsALot)){
        int raiseAmount = std::clamp(
            valueBetSize(0.75, basicState.potSize),
            basicState.minRaise,
            basicState.maxRaise
        );
        if(preflop) playedPreflopThisHand = true;
        firedBluffThisHand = true;
        ++state.betsFiredThisHand;
        state.lastRaiseWasBluff = true;
        return {Action::Raise, raiseAmount};
    }

    if(canCall && callDecisionMargin >= callBar && clearsCallFloor){
        if(preflop) playedPreflopThisHand = true;
        madeQualifyingCallThisHand = true;
        return {Action::Call, 0};
    }

    //short-stacked and facing a bet larger than the remaining stack - Call
    //isn't legal here, but calling all-in is the same decision mechanically,
    //just capped by the stack, so it uses the same equity-vs-pot-odds bar
    if(!canCall && canAllIn && amountToCall > 0 && callDecisionMargin >= callBar && clearsCallFloor){
        if(preflop) playedPreflopThisHand = true;
        madeQualifyingCallThisHand = true;
        return {Action::AllIn, 0};
    }

    if(canCheck){
        return {Action::Check, 0};
    }

    foldedThisHand = true;
    return {Action::Fold, 0};
}

void BalancedCPU::ensureTracked(int id){
    std::size_t required = static_cast<std::size_t>(id) + 1;

    if(opponentStats.size() < required){
        opponentStats.resize(required);
    }
    if(vpipCountedThisHand.size() < required){
        vpipCountedThisHand.resize(required);
    }
    if(handsSeenByPlayer.size() < required){
        handsSeenByPlayer.resize(required);
    }
    if(seenThisHand.size() < required){
        seenThisHand.resize(required);
    }
    if(streetContext.checkedThisStreet.size() < required){
        streetContext.checkedThisStreet.resize(required);
    }
}

void BalancedCPU::handleStreetTransition(const std::vector<Card>& board){
    int boardSize = static_cast<int>(board.size());

    if(boardSize == streetContext.lastSeenBoardSize){
        return;
    }

    streetContext.lastSeenBoardSize = boardSize;
    streetContext.raisesThisStreet = 0;
    streetContext.cBetFiredThisStreet = false;
    std::fill(streetContext.checkedThisStreet.begin(), streetContext.checkedThisStreet.end(), false);
}

//boardHasFlushDraw/boardHasStraightDraw/boardWetness moved to the anonymous
//namespace near boardIsPaired, so they're shared with AdvancedCPU - unqualified
//calls to boardWetness() below (in observeShowdown) still resolve there

void BalancedCPU::resetForNewHand(){
    CPU::resetForNewHand();

    //learn from the hand that just ended before its flags get cleared below -
    //skipped on the constructor-time initial reset, before hand 1 is even
    //dealt, since there's nothing to learn from yet
    if(handsObserved > 0){
        adapt();
    }

    playedPreflopThisHand = false;
    firedBluffThisHand = false;
    firedValueRaiseThisHand = false;
    madeQualifyingCallThisHand = false;
    foldedThisHand = false;
    reachedOwnShowdownThisHand = false;
    wonOwnShowdownThisHand = false;

    ++handsObserved;
    std::fill(vpipCountedThisHand.begin(), vpipCountedThisHand.end(), false);
    std::fill(seenThisHand.begin(), seenThisHand.end(), false);
    lastAggressorIdThisHand = -1;

    streetContext.lastSeenBoardSize = -1;
    streetContext.raisesThisStreet = 0;
    streetContext.cBetFiredThisStreet = false;
    std::fill(streetContext.checkedThisStreet.begin(), streetContext.checkedThisStreet.end(), false);
}

void BalancedCPU::adapt(){
    HandOutcome outcome;
    if(reachedOwnShowdownThisHand){
        outcome = wonOwnShowdownThisHand ? HandOutcome::Won : HandOutcome::Lost;
    }
    else if(foldedThisHand){
        outcome = HandOutcome::Folded;
    }
    else{
        //never folded, never reached showdown - won uncontested (includes
        //an all-in that took the pot down without anyone left to call)
        outcome = HandOutcome::Won;
    }

    if(playedPreflopThisHand){
        double nudge = outcome == HandOutcome::Won ? 0.97
                     : outcome == HandOutcome::Lost ? 1.08
                                                     : 1.02;
        //upper bound capped below decent-hand equity (J-10 offsuit is
        //~56%, a hand that's playable from anywhere short of facing real
        //aggression) - a run of bad variance should be able to tighten this
        //instance up noticeably, but a "balanced" personality adapting to a
        //losing streak should never become tight enough to fold a hand like
        //J-10 or AK facing nothing but the big blind. 0.60 (the previous
        //bound) was still tight enough to fold J-10o after enough losses;
        //0.75 (the original bound, before AK started folding) was tight
        //enough to fold AK itself after as few as 3-4 straight showdown
        //losses (each loss multiplies by 1.08)
        state.parameters.startingHandThreshold = std::clamp(
            state.parameters.startingHandThreshold * nudge, 0.30, 0.55
        );
    }

    if(firedBluffThisHand){
        //reacts hardest to a loss - if bluffing isn't working, chill on it
        double nudge = outcome == HandOutcome::Won ? 1.05
                     : outcome == HandOutcome::Lost ? 0.85
                                                     : 1.0;
        state.parameters.bluffFrequency = std::clamp(
            state.parameters.bluffFrequency * nudge, 0.0, 0.5
        );
    }

    if(firedValueRaiseThisHand){
        double nudge = outcome == HandOutcome::Won ? 0.97
                     : outcome == HandOutcome::Lost ? 1.08
                                                     : 1.02;
        //0.35 (unbounded relative to any other personality) let a losing
        //streak push this well above TightCPU's own fixed, deliberately-
        //strict 0.18 - the same "a balanced personality's worst adapted
        //state should never be tighter than the dedicated tight one"
        //principle startingHandThreshold's clamp above already enforces,
        //just never applied here. 0.17 sits just under TightCPU's 0.18
        state.parameters.raiseMargin = std::clamp(
            state.parameters.raiseMargin * nudge, 0.01, 0.17
        );
    }

    if(madeQualifyingCallThisHand){
        double nudge = outcome == HandOutcome::Won ? 0.97
                     : outcome == HandOutcome::Lost ? 1.08
                                                     : 1.02;
        //same reasoning as raiseMargin above - 0.10 sits just under
        //TightCPU's own fixed callMargin of 0.11
        state.parameters.callMargin = std::clamp(
            state.parameters.callMargin * nudge, 0.01, 0.10
        );
    }

    //tracks the hand's overall outcome regardless of which specific
    //behavior was engaged, since it's a general confidence dial rather
    //than tied to one action type
    double riskNudge = outcome == HandOutcome::Won ? 1.03
                      : outcome == HandOutcome::Lost ? 0.95
                                                      : 1.0;
    state.parameters.riskTolerance = std::clamp(
        state.parameters.riskTolerance * riskNudge, 0.0, 0.9
    );
}

void BalancedCPU::observeAction(const Player& actor, const PlayerAction& action,
                                 const BasicCPUState& stateBeforeAction){
    CPU::observeAction(actor, action, stateBeforeAction);

    int id = actor.getID();
    ensureTracked(id);
    handleStreetTransition(stateBeforeAction.board);

    std::size_t seenIdx = static_cast<std::size_t>(id);
    if(id != getID() && !seenThisHand[seenIdx]){
        seenThisHand[seenIdx] = true;
        ++handsSeenByPlayer[seenIdx];
    }

    //an all-in that increases the bet is raise-like; one that merely covers
    //(or falls short of) the existing bet is call-like - mirrors applyAction's
    //own newPlayerBet > currentHighestBet check exactly
    bool raiseLike = action.action == Action::Raise ||
        (action.action == Action::AllIn && actor.getCurrentBet() > stateBeforeAction.currentHighestBet);
    bool callLike = action.action == Action::Call ||
        (action.action == Action::AllIn && !raiseLike);

    bool preflop = stateBeforeAction.board.empty();
    bool isSelf = (id == getID());
    std::size_t idx = static_cast<std::size_t>(id);
    OpponentStats* stats = isSelf ? nullptr : &opponentStats[idx];

    if(preflop){
        if(action.action == Action::Fold){
            if(stats){
                ++stats->preflopFolds;
                if(streetContext.raisesThisStreet == 2){
                    ++stats->foldsToThreeBet;
                }
            }
        }
        else if(callLike){
            if(stats){
                ++stats->preflopCalls;
                if(stateBeforeAction.currentHighestBet == Player::bigBlind){
                    ++stats->limps;
                }
            }
            if(!vpipCountedThisHand[idx]){
                vpipCountedThisHand[idx] = true;
                if(stats) ++stats->voluntaryHandsPlayed;
            }
        }
        else if(raiseLike){
            if(stats) ++stats->preflopRaises;

            if(!vpipCountedThisHand[idx]){
                vpipCountedThisHand[idx] = true;
                if(stats) ++stats->voluntaryHandsPlayed;
            }

            ++streetContext.raisesThisStreet;
            if(streetContext.raisesThisStreet == 2 && stats){
                ++stats->threeBets;
            }

            if(stats && stateBeforeAction.potSize > 0){
                double raiseIncrement = static_cast<double>(
                    actor.getCurrentBet() - stateBeforeAction.currentHighestBet
                );
                stats->totalRaisePotRatio += raiseIncrement / stateBeforeAction.potSize;
            }

            lastAggressorIdThisHand = id;
        }
        //preflop Check (the big blind's free option) isn't voluntary and has no field - not tracked
    }
    else{
        bool wasCBetOpportunity = (id == lastAggressorIdThisHand && stateBeforeAction.currentHighestBet == 0);

        if(action.action == Action::Check){
            if(stats) ++stats->checks;
            streetContext.checkedThisStreet[idx] = true;
            if(wasCBetOpportunity && stats){
                ++stats->continuationBetOpportunities;
            }
        }
        else if(action.action == Action::Fold){
            if(stats){
                ++stats->folds;
                if(streetContext.cBetFiredThisStreet && streetContext.raisesThisStreet == 1
                   && stateBeforeAction.currentHighestBet > 0){
                    ++stats->foldsToContinuationBet;
                }
            }
        }
        else if(callLike){
            if(stats) ++stats->calls;
        }
        else if(raiseLike){
            bool openingBet = (stateBeforeAction.currentHighestBet == 0);
            double raiseIncrement = static_cast<double>(
                actor.getCurrentBet() - stateBeforeAction.currentHighestBet
            );
            double potRatio = stateBeforeAction.potSize > 0
                ? raiseIncrement / stateBeforeAction.potSize : 0.0;

            if(openingBet){
                if(stats){
                    ++stats->bets;
                    stats->totalBetPotRatio += potRatio;
                }
                if(wasCBetOpportunity){
                    if(stats){
                        ++stats->continuationBetOpportunities;
                        ++stats->continuationBets;
                    }
                    streetContext.cBetFiredThisStreet = true;
                }
            }
            else{
                if(stats){
                    ++stats->raises;
                    stats->totalRaisePotRatio += potRatio;
                    if(streetContext.checkedThisStreet[idx]){
                        ++stats->checkRaises;
                    }
                }
            }

            ++streetContext.raisesThisStreet;
            lastAggressorIdThisHand = id;
        }
    }
}

void BalancedCPU::observeShowdown(const Player& shower, const std::vector<Card>& board,
                                   const HandValue& shownValue, bool won){
    int id = shower.getID();

    if(id == getID()){
        //self-adaptation reads this via adapt(), not the opponentStats table
        reachedOwnShowdownThisHand = true;
        wonOwnShowdownThisHand = won;
        return;
    }

    ensureTracked(id);

    OpponentStats& stats = opponentStats[static_cast<std::size_t>(id)];
    ++stats.showdowns;
    if(won){
        ++stats.showdownWins;
    }

    //only a hand that got to showdown by being the last aggressor (rather
    //than just calling/checking along) is meaningfully a "bluff" or "value"
    //show - the strength required to count as value scales with how wet the
    //board is, since e.g. a bare pair means something very different on a
    //dry board than on one where straights and flushes are both live
    if(id != lastAggressorIdThisHand){
        return;
    }

    int wetness = boardWetness(board);
    HandCategory valueThreshold = wetness == 0 ? HandCategory::onePair
                                : wetness == 1 ? HandCategory::twoPair
                                                : HandCategory::straight;

    if(shownValue.category >= valueThreshold){
        ++stats.revealedValueBets;
    }
    else{
        ++stats.revealedBluffs;
    }
}

BalancedCPU::OpponentRead BalancedCPU::readOpponent(int id, bool preflop) const{
    OpponentRead read;

    if(id < 0 || id == getID() || static_cast<std::size_t>(id) >= handsSeenByPlayer.size()){
        return read;
    }

    //total hands actually observed for this opponent - the denominator both
    //ratios below are normalized against, since a raw count like "folded 5
    //times" means something very different out of 6 hands seen than out of
    //100, and is also this read's confidence gate (see below)
    int seen = handsSeenByPlayer[static_cast<std::size_t>(id)];
    double confidence = std::clamp(seen / static_cast<double>(OPPONENT_FULL_CONFIDENCE_HANDS), 0.0, 1.0);
    if(confidence <= 0.0){
        return read;
    }

    const OpponentStats& s = opponentStats[static_cast<std::size_t>(id)];

    double foldFreq;
    double looseness; //VPIP preflop, postflop bet/raise frequency otherwise
    double xFoldRate;  //fold-to-3bet preflop, fold-to-continuation-bet postflop
    double foldBaseline;
    if(preflop){
        //normalized by hands seen, not decisions faced - every hand this
        //opponent is dealt in gives them a preflop fold/play decision (modulo
        //an untracked free BB check), unlike postflop where a folded-out hand
        //never reaches them at all
        foldFreq = seen >= 5 ? static_cast<double>(s.preflopFolds) / seen : 0.60;
        looseness = seen >= 5 ? static_cast<double>(s.voluntaryHandsPlayed) / seen : 0.40;
        xFoldRate = s.threeBets >= 3
            ? static_cast<double>(s.foldsToThreeBet) / s.threeBets : 0.5;
        foldBaseline = 0.60;
    }
    else{
        //default to a neutral 35% fold / 25% aggression baseline until this
        //opponent has enough postflop decisions of their own to trust
        int postflopActs = s.bets + s.calls + s.raises + s.checks + s.folds;
        foldFreq = postflopActs >= 5 ? static_cast<double>(s.folds) / postflopActs : 0.35;
        looseness = postflopActs >= 5
            ? static_cast<double>(s.bets + s.raises) / postflopActs : 0.25;
        xFoldRate = s.continuationBetOpportunities >= 3
            ? static_cast<double>(s.foldsToContinuationBet) / s.continuationBetOpportunities : 0.5;
        foldBaseline = 0.35;
    }

    //an opponent who is rarely the one betting/raising (postflop) or rarely
    //volunteers to play a hand at all (preflop VPIP) means it when they
    //finally do show aggression - give it more credit; a habitually
    //loose/aggressive one means it less - call/raise over them lighter
    double credibility = std::clamp(1.35 - looseness * 1.4, 0.7, 1.35);

    //an opponent who folds a lot (especially to a 3-bet/continuation bet) is
    //a green light to bluff/float more; one who rarely folds is a calling
    //station - bluffing them is wasted chips, so bluff them less instead
    double foldSignal = std::clamp((foldFreq - foldBaseline) * 1.5 + (xFoldRate - 0.5), -0.5, 0.5);

    //a showdown-revealed bluff/value classification (observeShowdown) is
    //direct ground truth about this specific opponent's bluffing tendency,
    //rather than an inference from how often they fold - blended in on top
    //of foldSignal above, weighted by its own sample-based confidence since
    //showdowns are rare and this should stay modest until several have
    //actually been observed. Previously tracked but only consulted for one
    //narrow exemption (bluffNotWorkingPenalty's opponentBluffsALot check in
    //act()) - this is what actually makes the read "track revealed behavior"
    int revealedShows = s.revealedBluffs + s.revealedValueBets;
    double revealedConfidence = std::clamp(revealedShows / 6.0, 0.0, 1.0);
    double revealedSignal = revealedShows > 0
        ? std::clamp((static_cast<double>(s.revealedBluffs) / revealedShows - 0.5) * 1.6, -0.5, 0.5)
        : 0.0;

    read.callBarMult = 1.0 + (credibility - 1.0) * confidence;
    read.raiseBarMult = 1.0 + ((credibility - 1.0) * 0.5 - foldSignal * 0.3) * confidence;
    read.bluffMult = std::clamp(
        1.0 + foldSignal * confidence + revealedSignal * revealedConfidence, 0.3, 1.8
    );

    return read;
}

CPUParameters BalancedCPU::getParameters() const{
    return state.parameters;
}

int BalancedCPU::getHandsObserved(){
    return handsObserved;
}

OpponentStats BalancedCPU::getStats(int id){
    ensureTracked(id);
    return opponentStats[static_cast<std::size_t>(id)];
}

//shared across every AdvancedCPU instance, since they all observe the exact
//same public action stream - there's no benefit to each maintaining an
//independent copy, only redundant bookkeeping (same rationale as
//BalancedCPU's static tracking above)
std::vector<AdvancedCPU::ActionRecord> AdvancedCPU::actionHistory;
long long AdvancedCPU::lastLoggedActionSeq = -1;
std::vector<int> AdvancedCPU::lastHandSeenForPlayer;
std::vector<int> AdvancedCPU::handsSeenForPlayer;

int AdvancedCPU::handsSeenCount(int id){
    if(id < 0 || static_cast<std::size_t>(id) >= handsSeenForPlayer.size()){
        return 0;
    }
    return handsSeenForPlayer[static_cast<std::size_t>(id)];
}

void AdvancedCPU::ensureHandsSeenTracked(int id){
    std::size_t required = static_cast<std::size_t>(id) + 1;

    if(lastHandSeenForPlayer.size() < required){
        //-1 so the first hand actually observed (handNumber >= 1) always
        //counts as new, regardless of what handNumber happens to be
        lastHandSeenForPlayer.resize(required, -1);
    }
    if(handsSeenForPlayer.size() < required){
        handsSeenForPlayer.resize(required, 0);
    }
}

AdvancedCPU::AdvancedCPU(const std::string &nameIn, int chipsIn, int playerIDin)
    : CPU(nameIn, chipsIn, playerIDin){
    //+-5%, narrower than BalancedCPU's +-15% - a decent-but-not-premium
    //preflop hand (e.g. unsuited J-10, ~56% heads-up equity) needs to clear
    //startingHandThreshold comfortably at the intended ~0.51 baseline, but a
    //wider jitter let an individual instance's actual threshold drift as
    //high as ~0.56, turning a hand that should be a clear, easy continue
    //into a near coin flip purely because that one bot got unlucky on its
    //jitter roll - this still gives every instance some individuality
    //without letting any single one drift meaningfully tighter than intended
    auto jitter = [](double base){ return base * (0.95 + randomChance() * 0.10); };

    //baseline sits between TightCPU's and BlufferCPU's, same as BalancedCPU's -
    //an AdvancedCPU with no history yet on the table should play like a
    //straightforward balanced player, not lean tight or loose before it has
    //any real data to deviate from that baseline with
    state.parameters.startingHandThreshold = jitter(0.51);
    //0.06 -> 0.085 baseline, same reasoning as TightCPU/BlufferCPU above -
    //too low to ever meaningfully fold once a hand clears the preflop screen
    state.parameters.callMargin = jitter(0.085);
    state.parameters.raiseMargin = jitter(0.14);
    //note: no bluffFrequency here - AdvancedCPU derives its bluff rate from
    //gtoIndifferenceRatio() (the bet's actual size) instead of a flat tuned
    //constant, see act()
    state.parameters.riskTolerance = jitter(0.13);
}

PlayerAction AdvancedCPU::act(const BasicCPUState& basicState){
    Equity equityCalc(hand, basicState.board, CPU_SIMULATIONS, basicState.playersInHand - 1);
    EquityResult result = equityCalc.calculate();

    bool canRaise = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                              Action::Raise) != basicState.availableActions.end();
    bool canCall = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                             Action::Call) != basicState.availableActions.end();
    bool canCheck = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                              Action::Check) != basicState.availableActions.end();
    bool canAllIn = std::find(basicState.availableActions.begin(), basicState.availableActions.end(),
                              Action::AllIn) != basicState.availableActions.end();

    int amountToCall = basicState.currentHighestBet - currentBet;

    //preflop hands are screened by starting-hand strength before pot odds are even considered
    bool preflop = basicState.board.empty();

    //whether this decision is genuinely responding to a raise/bet, for
    //discountForAggression() below - facing only the big blind preflop
    //(amountToCall > 0 purely from the cost of entering the pot, nobody
    //having actually raised) does NOT count, even though amountToCall is
    //still positive there; blinds are posted directly in Game::preFlop(),
    //never through the act()/observeAction() loop, so state.raisesFacedThisStreet
    //stays 0 until a real raise happens - same "has anyone actually raised"
    //test openPriceLooseness() already applies to the starting-hand
    //threshold. Postflop, currentHighestBet resets to 0 each street, so any
    //positive amountToCall there is inherently a real bet - no blind-
    //equivalent noise to filter out
    bool facingRealAggression = preflop
        ? (amountToCall > 0 && basicState.currentHighestBet > Player::bigBlind)
        : (amountToCall > 0);
    BettingStreet street = streetFromBoard(basicState.board);

    //basicState.lastAggressor already tracks whoever was last aggressive
    //anywhere in the hand so far - Game only updates it on a raise-like
    //action, and seeds it to the big blind poster preflop - so unlike
    //BalancedCPU, no separate same-hand bookkeeping is needed here to find
    //"who this decision is really about"
    int opponentId = (basicState.lastAggressor != nullptr && basicState.lastAggressor != this)
        ? basicState.lastAggressor->getID() : -1;

    ExploitRead read = buildExploitRead(opponentId, street);

    if(preflop && amountToCall > 0){
        double preflopThreshold = state.parameters.startingHandThreshold
            * positionalLooseness(basicState.position)
            * openPriceLooseness(basicState.currentHighestBet);
        double preflopStrength = preflopHandStrength(hand);

        //a hard deterministic cutoff here has an exact, learnable line - the
        //same indifference-point mixing used below for the raise/call
        //decisions applies just as much to which hands are worth continuing
        //preflop at all, so a hand right at this instance's own
        //starting-hand bar doesn't fold/continue 100% predictably. Uses
        //ADVANCED_PREFLOP_MIX_WIDTH (not ADVANCED_MIX_WIDTH) since equity and
        //startingHandThreshold are both on a much larger scale than a raise/
        //call margin - see that constant's comment
        bool belowScreen = randomChance() >= mixedStrategyProbability(
            preflopStrength, preflopThreshold, ADVANCED_PREFLOP_MIX_WIDTH
        );

        if(belowScreen){
            int bluffRaiseAmount = std::clamp(
                valueBetSize(0.75, basicState.potSize),
                basicState.minRaise,
                basicState.maxRaise
            );

            //closed-form GTO base rate for THIS bet's actual size, in place of a
            //flat tuned constant - riskTolerance decides how much this instance
            //leans on the theoretical number, then the same situational
            //dampeners every personality already uses layer an exploitative
            //deviation on top of that GTO baseline
            double bluffChance = std::clamp(
                gtoIndifferenceRatio(static_cast<double>(basicState.potSize), static_cast<double>(bluffRaiseAmount))
                    * state.parameters.riskTolerance * read.bluffMult / reraiseFatigue(state.raisesFacedThisStreet),
                0.0, 0.6
            ) * boardDangerDampening(basicState.board) * allInBluffDampening(basicState) * manyCallersBluffDampening(basicState)
              / bluffNotWorkingPenalty(state.bluffCalledThisHand, false);

            if(canRaise && randomChance() < bluffChance){
                ++state.betsFiredThisHand;
                state.lastRaiseWasBluff = true;
                return {Action::Raise, bluffRaiseAmount};
            }
            return {Action::Fold, 0};
        }
    }

    //past the starting-hand screen (or postflop), continue based on equity versus pot odds -
    //leading out (nothing to call) uses OPENING_BET_EQUITY_FLOOR in place of real pot odds,
    //since "any equity above zero" is not a real bar for betting when nobody's making you
    double potOdds = amountToCall > 0
        ? static_cast<double>(amountToCall) / (basicState.potSize + amountToCall)
        : OPENING_BET_EQUITY_FLOOR;
    //preflop specifically, the RAISE decision needs a more realistic
    //opponent count than "literally everyone still dealt into the hand,"
    //for the exact same reason CALLING (below) uses heads-up equity instead
    //of raw multiway - see preflopRaiseEquity()'s comment
    double raiseEquityBasis = discountForAggression(
        preflop ? preflopRaiseEquity(hand, basicState) : result.equity,
        basicState.board, facingRealAggression, state.raisesFacedThisStreet
    );
    double margin = raiseEquityBasis - potOdds;

    double callEquityBasis = discountForAggression(
        discountForAllIn(preflop ? preflopHandStrength(hand) : result.equity, basicState),
        basicState.board, facingRealAggression, state.raisesFacedThisStreet
    );
    double callDecisionMargin = callEquityBasis - potOdds;

    bool clearsCallFloor = !preflop || callEquityBasis >= state.parameters.startingHandThreshold * 0.85;

    //a genuine bet-SIZING tell on the specific opponent behind THIS bet, not
    //just a blanket per-street average: what has this player's history shown
    //about bets sized like the one actually being faced right now? Neutral
    //(1.0) until there's enough matching-sized history on them to trust
    double sizingTellMult = 1.0;
    if(amountToCall > 0 && opponentId >= 0){
        double priorPotForSizing = static_cast<double>(basicState.potSize - amountToCall);
        if(priorPotForSizing > 0.0){
            double betToPotRatio = static_cast<double>(amountToCall) / priorPotForSizing;
            double sizingBluffRate = sizingTellBluffRate(opponentId, betToPotRatio);
            if(sizingBluffRate >= 0.0){
                //bets of this size have, for this specific opponent, meant
                //bluff more or less often than a neutral 50/50 read would
                //assume - lower the bars when this sizing usually means
                //bluff, raise them when it usually means real value
                sizingTellMult = std::clamp(1.0 + (0.5 - sizingBluffRate) * 1.2, 0.4, 1.6);
            }
        }
    }

    //real seat position (not just the playersYetToAct proxy situationalDifficulty
    //uses) - reuses positionalLooseness() postflop too (preflop already applies
    //it to the starting-hand screen threshold, so applying it again there would
    //double-count); early position stays at baseline, late position gets a
    //discount, matching standard "play tighter early, looser late" position play
    double positionMult = preflop ? 1.0 : positionalLooseness(basicState.position);

    //the bar to clear scales with position, live callers, the aggressor's stack, pot commitment,
    //how many streets this hand this CPU has already bet/raised on, and (unlike TightCPU/BlufferCPU)
    //a ground-truth exploit read on the specific opponent driving this decision
    double raiseBar = state.parameters.raiseMargin
        * situationalDifficulty(basicState, this, state.parameters.riskTolerance, true)
        * barrelFatigue(state.betsFiredThisHand, street)
        * reraiseFatigue(state.raisesFacedThisStreet)
        * read.raiseBarMult
        * sizingTellMult
        * positionMult
        * bluffNotWorkingPenalty(state.bluffCalledThisHand, false);
    double callBar = state.parameters.callMargin
        * situationalDifficulty(basicState, this, state.parameters.riskTolerance, false)
        * read.callBarMult
        * sizingTellMult
        * positionMult
        * (facingRealAggression ? multiwayCallCaution(basicState.playersInHand) : 1.0)
        * bluffNotWorkingPenalty(state.bluffCalledThisHand, false);

    if(canRaise && randomChance() < mixedStrategyProbability(margin, raiseBar, ADVANCED_MIX_WIDTH)
       && !shouldSlowplay(state, street, state.raisesFacedThisStreet)){
        int raiseAmount = std::clamp(
            valueBetSize(raiseEquityBasis, basicState.potSize),
            basicState.minRaise,
            basicState.maxRaise
        );
        ++state.betsFiredThisHand;
        state.lastRaiseWasBluff = false;
        return {Action::Raise, raiseAmount};
    }

    int postflopBluffRaiseAmount = std::clamp(
        valueBetSize(0.75, basicState.potSize),
        basicState.minRaise,
        basicState.maxRaise
    );
    double postflopBluffChance = std::clamp(
        gtoIndifferenceRatio(static_cast<double>(basicState.potSize), static_cast<double>(postflopBluffRaiseAmount))
            * state.parameters.riskTolerance * read.bluffMult / reraiseFatigue(state.raisesFacedThisStreet),
        0.0, 0.6
    ) * boardDangerDampening(basicState.board) * allInBluffDampening(basicState) * manyCallersBluffDampening(basicState)
      / bluffNotWorkingPenalty(state.bluffCalledThisHand, false);

    if(canRaise && randomChance() < postflopBluffChance){
        ++state.betsFiredThisHand;
        state.lastRaiseWasBluff = true;
        return {Action::Raise, postflopBluffRaiseAmount};
    }

    bool clearedByMargin = randomChance() < mixedStrategyProbability(callDecisionMargin, callBar, ADVANCED_MIX_WIDTH);

    //Minimum Defense Frequency: even when a hand doesn't clear this
    //personality's own (exploitative) comfort bar, folding EVERY such hand
    //would make this bot exploitable by any opponent betting into it with
    //literally any two cards - a border hand that's still genuinely
    //profitable in isolation (clears real pot odds, just not the stricter
    //tuned bar) continues with probability equal to the closed-form minimum
    //defense frequency for this bet's actual size, mixing a real GTO-derived
    //frequency into what's otherwise an exploit-driven decision. Restricted
    //to heads-up (exactly one live opponent): the MDF formula comes from a
    //single-bettor-vs-single-defender indifference equation - it doesn't
    //apply as-is once other players can also defend the same bet, since no
    //single multiway defender needs to personally hit the full heads-up rate
    bool clearedByMDF = false;
    if(!clearedByMargin && amountToCall > 0 && callEquityBasis > potOdds && basicState.playersInHand <= 2){
        double priorPot = static_cast<double>(basicState.potSize - amountToCall);
        double mdf = 1.0 - gtoIndifferenceRatio(priorPot, static_cast<double>(amountToCall));
        clearedByMDF = randomChance() < mdf;
    }

    if(canCall && clearsCallFloor && (clearedByMargin || clearedByMDF)){
        return {Action::Call, 0};
    }

    //short-stacked and facing a bet larger than the remaining stack - Call
    //isn't legal here, but calling all-in is the same decision mechanically,
    //just capped by the stack, so it uses the same bars (equity-vs-pot-odds
    //margin, or, at the margin, Minimum Defense Frequency)
    if(!canCall && canAllIn && amountToCall > 0 && clearsCallFloor && (clearedByMargin || clearedByMDF)){
        return {Action::AllIn, 0};
    }

    if(canCheck){
        return {Action::Check, 0};
    }

    return {Action::Fold, 0};
}

void AdvancedCPU::observeAction(const Player& actor, const PlayerAction& action,
                                 const BasicCPUState& stateBeforeAction){
    CPU::observeAction(actor, action, stateBeforeAction);

    //Game::round notifies every player object (not just this one) after
    //every action, so with more than one AdvancedCPU at the table this would
    //otherwise log the same real action once per instance - actionSeq is
    //identical across every observer's call for the same action, so it
    //doubles as a safe de-duplication key
    if(stateBeforeAction.actionSeq == lastLoggedActionSeq){
        return;
    }
    lastLoggedActionSeq = stateBeforeAction.actionSeq;

    //tracks how many DISTINCT hands actor has been observed acting in - a
    //faster-accumulating, less noisy confidence signal (see
    //ADVANCED_FULL_CONFIDENCE_HANDS) than any of the narrower/sparser action
    //types (raise-like on one street, facing aggression) actionHistory below
    //is filtered by when buildExploitRead() builds its read
    int actorId = actor.getID();
    ensureHandsSeenTracked(actorId);
    std::size_t actorIdx = static_cast<std::size_t>(actorId);
    if(lastHandSeenForPlayer[actorIdx] != stateBeforeAction.handNumber){
        lastHandSeenForPlayer[actorIdx] = stateBeforeAction.handNumber;
        ++handsSeenForPlayer[actorIdx];
    }

    //the one piece of information no other personality logs: actor's TRUE
    //win-equity (their real hand, not a guess) at the moment of this action -
    //purely a historical fact from here on, never consulted for a hand
    //that's still live (see act(), which only ever runs Equity on `hand`,
    //this instance's own cards, same as every other personality)
    int opponentsInHand = std::max(1, stateBeforeAction.playersInHand - 1);
    Equity equityCalc(actor.getHand(), stateBeforeAction.board, ADVANCED_HISTORY_SIMULATIONS, opponentsInHand);
    double trueEquity = equityCalc.calculate().equity;

    actionHistory.push_back(ActionRecord{
        actor.getID(),
        streetFromBoard(stateBeforeAction.board),
        stateBeforeAction.position,
        action.action,
        actor.getCurrentBet(),
        stateBeforeAction.currentHighestBet,
        stateBeforeAction.potSize,
        stateBeforeAction.playersInHand,
        trueEquity
    });
}

AdvancedCPU::ExploitRead AdvancedCPU::buildExploitRead(int opponentId, BettingStreet street) const{
    ExploitRead read;

    if(opponentId < 0 || opponentId == getID()){
        return read;
    }

    int raiseSamples = 0;
    double equitySum = 0.0;

    int facedAggressionCount = 0;
    int foldCount = 0;

    for(const ActionRecord &record : actionHistory){
        if(record.playerID != opponentId){
            continue;
        }

        bool raiseLike = record.action == Action::Raise ||
            (record.action == Action::AllIn && record.postActionBet > record.currentHighestBetBefore);

        if(record.street == street && raiseLike){
            ++raiseSamples;
            equitySum += record.equityAtAction;
        }

        //fold-under-pressure is tracked across the whole hand's history (any
        //street), a coarser but simpler signal than a per-street one - enough
        //to tell a habitual folder from someone who never gives up a pot
        if(record.currentHighestBetBefore > 0){
            ++facedAggressionCount;
            if(record.action == Action::Fold){
                ++foldCount;
            }
        }
    }

    double handsSeenConfidence = std::clamp(
        static_cast<double>(handsSeenCount(opponentId)) / ADVANCED_FULL_CONFIDENCE_HANDS, 0.0, 1.0
    );

    if(raiseSamples > 0){
        //confidence tracks DISTINCT HANDS seen of this opponent, not the
        //(much sparser) raiseSamples count itself - a raise-like action on
        //one specific street is rare enough that gating confidence on it
        //directly would take far more hands than ADVANCED_FULL_CONFIDENCE_HANDS
        //to ever ramp up, even against an opponent who's genuinely well known
        //by now. raiseSamples > 0 still gates whether there's ANY read to
        //lean on at all - handsSeenConfidence only controls how hard to lean on it
        double confidence = handsSeenConfidence;
        double avgEquity = equitySum / raiseSamples;

        //raises averaging well above the value bar mean it when they raise -
        //demand more equity to continue over them; raises averaging at or
        //below it mean their aggression is driven by more than hand strength
        //(bluffs, protection, blockers) - call/raise back lighter. This is
        //ground truth (every raise they've ever made, correctly labeled),
        //not an estimate from the rare hands that actually reach showdown
        double credibility = std::clamp(0.6 + (avgEquity - ADVANCED_VALUE_EQUITY_BAR) * 1.6, 0.6, 1.6);
        read.callBarMult = 1.0 + (credibility - 1.0) * confidence;
        read.raiseBarMult = 1.0 + (credibility - 1.0) * 0.7 * confidence;
    }

    if(facedAggressionCount >= 3){
        double foldRate = static_cast<double>(foldCount) / facedAggressionCount;

        //folds under pressure often -> bluff/apply pressure on them more;
        //rarely folds -> a calling station, bluffing them is wasted chips
        read.bluffMult = std::clamp(1.0 + (foldRate - 0.45) * 1.8 * handsSeenConfidence, 0.3, 1.8);
    }

    return read;
}

double AdvancedCPU::gtoIndifferenceRatio(double potBeforeBet, double betSize){
    double denominator = potBeforeBet + betSize;
    if(denominator <= 0.0){
        return 0.0;
    }
    return betSize / denominator;
}

double AdvancedCPU::sizingTellBluffRate(int opponentId, double betToPotRatio) const{
    if(opponentId < 0 || opponentId == getID()){
        return -1.0;
    }

    int samples = 0;
    int bluffs = 0;

    for(const ActionRecord &record : actionHistory){
        if(record.playerID != opponentId || record.potSizeBefore <= 0){
            continue;
        }

        bool raiseLike = record.action == Action::Raise ||
            (record.action == Action::AllIn && record.postActionBet > record.currentHighestBetBefore);
        if(!raiseLike){
            continue;
        }

        double recordBetToPot = static_cast<double>(record.postActionBet - record.currentHighestBetBefore)
            / record.potSizeBefore;

        if(std::abs(recordBetToPot - betToPotRatio) > ADVANCED_SIZING_TELL_BAND){
            continue;
        }

        ++samples;
        if(record.equityAtAction < ADVANCED_VALUE_EQUITY_BAR){
            ++bluffs;
        }
    }

    if(samples < ADVANCED_SIZING_TELL_MIN_SAMPLES){
        return -1.0;
    }

    return static_cast<double>(bluffs) / samples;
}

double AdvancedCPU::mixedStrategyProbability(double margin, double bar, double width){
    if(width <= 0.0){
        return margin >= bar ? 1.0 : 0.0;
    }
    double z = (margin - bar) / width;
    return 1.0 / (1.0 + std::exp(-z));
}