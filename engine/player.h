#pragma once

#include <array>

#include <string>
#include <vector>

#include "card.h"
#include "handEval.h"

enum class Action;
class Player;

struct ActionHash{
    std::size_t operator()(Action action) const{
        return static_cast<std::size_t>(action);
    }
};

enum class BettingStreet{
    PreFlop,
    Flop,
    Turn,
    River
};

enum class Position{
    Button,
    SmallBlind,
    BigBlind,
    UnderTheGun,
    UnderTheGunPlus1,
    UnderTheGunPlus2,
    UnderTheGunPlus3,
    Lojack,
    Hijack,
    Cutoff
};

struct OpponentStats{
    //preflop tendencies
    int voluntaryHandsPlayed;
    int preflopRaises;
    int preflopCalls;
    int preflopFolds;
    int threeBets;
    int foldsToThreeBet;
    int limps;

    //postflop tendencies
    int bets;
    int calls;
    int raises;
    int checks;
    int folds;

    int continuationBets;
    int continuationBetOpportunities;
    int foldsToContinuationBet;
    int checkRaises;

    //showdown information
    int showdowns;
    int showdownWins;
    int revealedBluffs;
    int revealedValueBets;

    //bet sizing
    double totalBetPotRatio;
    double totalRaisePotRatio;

    //TODO: current-hand estimate
    //Range estimatedRange;
};

struct CPUParameters{
    //the win-equity a hand needs HEADS-UP AGAINST ONE RANDOM OPPONENT (not
    //vs. the real table size) before a CPU is willing to voluntarily enter
    //a pot preflop. Heads-up equity is used as the ruler here instead of
    //actual multiway equity because multiway equity gets squeezed toward
    //1/N as opponents are added or fold - a fixed bar against it would be
    //way too loose at a full table and unreachable at a short one. Heads-up
    //equity keeps a wide, table-size-independent ~15%-85% spread, so it
    //reads like a normal starting-hand range no matter how many are seated.
    //See preflopHandStrength() in player.cpp for more
    double startingHandThreshold;
    double callMargin;
    double raiseMargin;
    double bluffFrequency;
    double riskTolerance;
};

struct BasicCPUState{
    std::vector<Action> availableActions;
    std::vector<Card> board;

    Player* lastAggressor;//used to compare stack sizes

    int potSize;
    int currentHighestBet;
    int minRaise;
    int maxRaise;
    int playersInHand;
    int callersThisStreet;
    int playersYetToAct;

    Position position;

    //a counter Game::round bumps for every real action taken - unique to
    //THIS action, not to who's observing it. Since Game::round notifies
    //every player object once per action, a shared observer (see
    //AdvancedCPU::actionHistory) needs this to tell "the same action seen
    //twice" apart from "two different actions"
    int actionSeq = 0;

    //which hand this decision belongs to (Game::totalHands at the time,
    //1-indexed) - lets a shared observer count how many DISTINCT hands an
    //opponent has actually played, a cleaner confidence signal than
    //counting some narrower, sparser action type (see AdvancedCPU's
    //hands-seen-per-opponent tracking)
    int handNumber = 0;
};

struct CPUState{
    CPUParameters parameters;

    //hands of reduced bluffing left after firing one - set by whichever CPU
    //type tracks a bluff cooldown (currently just BlufferCPU), decayed once
    //per hand in that type's resetForNewHand() override
    int recentBluffHands = 0;

    //how many times this hand a raise-worthy hand got slowplayed (called or
    //checked) instead, to keep betting lines varied - reset per hand by
    //CPU::resetForNewHand()
    int slowplaysThisHand = 0;

    //how many streets this hand this player has bet or raised on (their own
    //aggression, not an opponent's) - makes repeated barreling need more and
    //more edge instead of firing every street on autopilot once ahead;
    //reset per hand by CPU::resetForNewHand()
    int betsFiredThisHand = 0;

    //how many raises (by anyone, including this player) the current street
    //has seen so far, tracked by CPU::observeAction() off the public action
    //stream. Monte Carlo equity treats opponents as holding random cards,
    //which stops being a fair assumption once someone's actually raised
    //(their real range is narrower and stronger) - this lets re-raising
    //require more edge the deeper a street already is, so two CPUs with
    //similar equity-vs-random don't just keep raising each other back
    //forever. Reset per hand by CPU::resetForNewHand() and per street by
    //CPU::observeAction() noticing the board grew
    int raisesFacedThisStreet = 0;

    //the board size CPU::observeAction() last saw, used to detect a new
    //street for raisesFacedThisStreet above - starts at -1 (via
    //CPU::resetForNewHand()) so the first action each hand always reads as
    //a fresh street
    int lastSeenBoardSizeForRaiseTracking = -1;

    //true when this player's last raise was a bluff, not a value raise -
    //set by act(), read and cleared by CPU::observeAction() once an
    //opponent responds, to tell whether the bluff worked (folded to) or
    //didn't (called/raised over)
    bool lastRaiseWasBluff = false;

    //true once an opponent calls or raises over (rather than folds to) a
    //bluff this player fired earlier in the hand - a real signal the story
    //isn't being bought, so pushing more chips in without a real hand needs
    //real justification afterward, not just "there's already a lot in the
    //pot." Set by CPU::observeAction(), reset per hand by
    //CPU::resetForNewHand()
    bool bluffCalledThisHand = false;
};

enum class PlayerType{
    TightCPU,
    BlufferCPU,
    BalancedCPU,
    AdvancedCPU,
};

struct PlayerAction{
    Action action;
    int amount = 0;//the raise size, if action is a raise
};

class Player{
    protected:
        std::array<Card, 2> hand;
        std::string name;
        HandValue handValue;
        int totalChips = 0;
        int currentBet = 0;//chips put in during the current betting round
        int totalChipsInHand = 0;//chips put in over the whole hand
        int playerID;
        bool inHand = true;
        bool actedSinceLastRaise = false;
    public:
        static int bigBlind;
        //MODIFIES: name, type, totalChips, playerID
        //EFFECTS: sets up the player with a name, starting stack, and id
        Player(const std::string &nameIn, int chipsIn, int playerIDin);

        virtual ~Player() = default;

        //MODIFIES: totalChips
        //EFFECTS: adds numChips to the stack
        void addChips(int numChips);

        //MODIFIES: totalChips, totalChipsInHand, currentBet
        //EFFECTS: moves numChips from the stack into the current bet
        void addToCurrentBet(int numChips);

        //EFFECTS: chips left in the stack
        int getNumChips() const;

        //EFFECTS: how much this player's put in this betting round
        int getCurrentBet() const;

        //EFFECTS: how much this player's put in over the whole hand
        int getTotalChipsInHand() const;

        //EFFECTS: this player's id
        int getID() const;

        //MODIFIES: hand
        //EFFECTS: deals card c into hole-card slot i
        void addCard(Card &c, int i);

        //EFFECTS: this player's hole cards
        const std::array<Card, 2>& getHand() const;

        //EFFECTS: true if this player has folded
        bool folded() const;

        //EFFECTS: true if this player is all in
        bool allIn() const;

        //EFFECTS: this player's name
        const std::string& getName() const;

        //MODIFIES: inHand
        //EFFECTS: folds the player out of the hand
        void fold();

        //MODIFIES: actedSinceLastRaise
        //EFFECTS: sets whether the player has responded to the latest raise yet
        void setActedSinceLastRaise(bool val);

        //MODIFIES: inHand, currentBet, actedSinceLastRaise, totalChipsInHand
        //EFFECTS: resets the player for a new hand
        virtual void resetForNewHand();

        //MODIFIES: currentBet, actedSinceLastRaise
        //EFFECTS: resets the player for a new betting round
        void resetForNewBettingRound();

        //EFFECTS: true if the player has already responded to the latest raise
        bool hasActedSinceLastRaise() const;

        //MODIFIES: player state depending on the chosen action
        //EFFECTS: asks the player to choose an action and returns it
        virtual PlayerAction act(const BasicCPUState& basicState) = 0;

        //EFFECTS: tells this player that actor just took action, with the
        //         table state as it stood right before that action - called
        //         for every player at the table (including actor) after
        //         every action. No-op by default; only opponent-modeling
        //         types (e.g. BalancedCPU) need to override it
        virtual void observeAction(const Player& actor, const PlayerAction& action,
                                    const BasicCPUState& stateBeforeAction);

        //EFFECTS: tells this player that shower's hand was revealed at
        //         showdown on the given complete board - called once per
        //         shower, for every player at the table. No-op by default.
        //         board is passed explicitly rather than reused from
        //         observeAction, since an all-in runout skips every action
        //         (and so every observeAction call) on streets nobody has
        //         to act on
        virtual void observeShowdown(const Player& shower, const std::vector<Card>& board,
                                      const HandValue& shownValue, bool won);
};

class CPU: public Player{
    public:
        //MODIFIES: inHand, currentBet, actedSinceLastRaise, totalChipsInHand,
        //          state.slowplaysThisHand, state.betsFiredThisHand,
        //          state.raisesFacedThisStreet, state.lastSeenBoardSizeForRaiseTracking,
        //          state.lastRaiseWasBluff, state.bluffCalledThisHand
        //EFFECTS: resets the per-hand CPU state shared by every personality -
        //         subclasses with extra per-hand state of their own
        //         (BlufferCPU, BalancedCPU) call this instead of
        //         Player::resetForNewHand() directly; TightCPU just
        //         inherits it as-is since it has nothing extra to reset
        void resetForNewHand() override;

        //MODIFIES: state.raisesFacedThisStreet, state.lastSeenBoardSizeForRaiseTracking,
        //          state.lastRaiseWasBluff, state.bluffCalledThisHand
        //EFFECTS: tracks how many raises the current street has seen, and
        //         whether an opponent called/raised over (rather than
        //         folded to) this player's last bluff - shared by every CPU
        //         personality; BalancedCPU overrides this for its own
        //         richer opponent modeling, calling this base version first
        void observeAction(const Player& actor, const PlayerAction& action,
                            const BasicCPUState& stateBeforeAction) override;

    protected:
        CPUState state{};

        //EFFECTS: sets up the underlying player
        CPU(const std::string &nameIn, int chipsIn, int playerIDin);
};

class TightCPU: public CPU{
    public:
        //EFFECTS: sets up a tight player's parameters
        TightCPU(const std::string &nameIn, int chipsIn, int playerIDin);

        PlayerAction act(const BasicCPUState& basicState) override;
};

class BlufferCPU: public CPU{
    public:
        //EFFECTS: sets up a bluffing player's parameters
        BlufferCPU(const std::string &nameIn, int chipsIn, int playerIDin);

        PlayerAction act(const BasicCPUState& basicState) override;

        //MODIFIES: inHand, currentBet, actedSinceLastRaise, totalChipsInHand,
        //          state.recentBluffHands
        //EFFECTS: resets for a new hand and lets the bluff cooldown tick down
        void resetForNewHand() override;

    private:
        //EFFECTS: the odds of firing a bluff right now - scales the base
        //         bluffFrequency down when fold equity looks weak (live
        //         callers, a multiway pot, early position, a pot-committed
        //         aggressor) and by state.recentBluffHands, so a recent
        //         bluff cools off for a couple hands before bluffing at the
        //         usual rate again
        double bluffChance(const BasicCPUState& basicState) const;
};

class BalancedCPU: public CPU{
    public:
        //EFFECTS: sets up the underlying player
        BalancedCPU(const std::string &nameIn, int chipsIn, int playerIDin);

        //MODIFIES: playedPreflopThisHand, firedBluffThisHand, firedValueRaiseThisHand,
        //          madeQualifyingCallThisHand, foldedThisHand, state.betsFiredThisHand
        //EFFECTS: picks an action - same equity-vs-pot-odds approach as
        //         TightCPU/BlufferCPU, but with its bars additionally scaled
        //         by a read on the relevant opponent (readOpponent()),
        //         blended toward neutral until there's enough history on them
        PlayerAction act(const BasicCPUState& basicState) override;

        //MODIFIES: inHand, currentBet, actedSinceLastRaise, totalChipsInHand,
        //          handsObserved, vpipCountedThisHand, seenThisHand,
        //          lastAggressorIdThisHand, streetContext
        //EFFECTS: resets for a new hand and advances the shared (class-wide)
        //         opponent-tracking bookkeeping
        void resetForNewHand() override;

        //MODIFIES: opponentStats, vpipCountedThisHand, handsSeenByPlayer,
        //          seenThisHand, lastAggressorIdThisHand, streetContext,
        //          state.raisesFacedThisStreet, state.lastSeenBoardSizeForRaiseTracking
        //EFFECTS: folds actor's action into the shared opponent-tracking
        //         tables - calls CPU::observeAction() first for the base
        //         raise-tracking every personality shares
        void observeAction(const Player& actor, const PlayerAction& action,
                            const BasicCPUState& stateBeforeAction) override;

        //MODIFIES: opponentStats, reachedOwnShowdownThisHand, wonOwnShowdownThisHand
        //EFFECTS: folds shower's revealed hand into the shared
        //         opponent-tracking tables, or (when shower is this
        //         instance) records its own showdown result for adapt() instead
        void observeShowdown(const Player& shower, const std::vector<Card>& board,
                              const HandValue& shownValue, bool won) override;

        //EFFECTS: this instance's current (jittered, self-adapting) parameters
        CPUParameters getParameters() const;

        //EFFECTS: how many hands have been observed so far
        static int getHandsObserved();

        //EFFECTS: the tracked stats for player id, or default (all-zero)
        //         stats if id hasn't been observed yet
        static OpponentStats getStats(int id);

    private:
        //tracking data is shared across every BalancedCPU instance (static)
        //rather than kept per-instance, since every instance watches the
        //exact same public action stream - separate copies would just be
        //redundant bookkeeping
        static int handsObserved;
        static std::vector<OpponentStats> opponentStats;//indexed by getID(), grown lazily
        static std::vector<bool> vpipCountedThisHand;//indexed by getID(), reset per hand

        //how many hands each id has actually been observed playing - unlike
        //handsObserved (this bot's own hand count), this is per-opponent, so
        //a read on one opponent can stay neutral until there's enough
        //history on THEM specifically, not just on the game in general
        static std::vector<int> handsSeenByPlayer;//indexed by getID(), grown lazily
        static std::vector<bool> seenThisHand;//indexed by getID(), reset per hand

        //who was last aggressive anywhere in the hand so far (not just this
        //street) - used for both continuation-bet detection and the
        //showdown bluff/value classification
        static int lastAggressorIdThisHand;

        //bookkeeping scoped to the current betting round, reset whenever
        //handleStreetTransition notices the board has grown
        struct StreetContext{
            int lastSeenBoardSize = -1;
            int raisesThisStreet = 0;
            bool cBetFiredThisStreet = false;
            std::vector<bool> checkedThisStreet;//indexed by getID(), reset per street
        };
        static StreetContext streetContext;

        //MODIFIES: opponentStats, vpipCountedThisHand, handsSeenByPlayer,
        //          seenThisHand, streetContext.checkedThisStreet
        //EFFECTS: grows the id-indexed tracking tables so id is safe to index
        static void ensureTracked(int id);

        //MODIFIES: streetContext
        //EFFECTS: resets per-street bookkeeping once the board shows a new street
        static void handleStreetTransition(const std::vector<Card>& board);

        //boardHasFlushDraw/boardHasStraightDraw/boardWetness live as free
        //functions shared with AdvancedCPU (see player.cpp's anonymous
        //namespace, near boardIsPaired) - the unqualified calls to
        //boardWetness() below still resolve there

        //multipliers this bot applies to its own bars/bluff frequency based
        //on a read on one specific opponent - all default to 1.0 (neutral,
        //defer entirely to equity/position/pot-odds) and blend toward their
        //read value based on how much data's actually been seen on that
        //opponent, per readOpponent()
        struct OpponentRead{
            double callBarMult = 1.0; //>1: their aggression is credible - demand more edge to call it
            double raiseBarMult = 1.0;//>1: harder to justify raising into/over them
            double bluffMult = 1.0;   //>1: they fold a lot - bluff/float them more; <1: calling station, bluff less
        };

        //EFFECTS: a read on opponent id, blended toward neutral (all
        //         multipliers 1.0) the fewer hands have been seen of them -
        //         falls back to pure equity/position judgement early in a
        //         session or against a stranger, and leans harder on tracked
        //         tendencies once there's enough history to trust. preflop
        //         picks which tendencies the read draws on: preflop fold
        //         rate/VPIP (both normalized by total hands actually seen
        //         for id, since folding 5 times means something very
        //         different out of 6 hands than out of 100) and
        //         fold-to-3bet rate preflop, vs. postflop fold/aggression
        //         frequency and fold-to-continuation-bet rate otherwise -
        //         mixing up which betting round's signals feed the read
        //         would read the wrong thing
        OpponentRead readOpponent(int id, bool preflop) const;

        //--- self-adaptation state below is per-instance (NOT static) - each
        //bot's own recent hand history is personal to it, unlike the shared
        //opponentStats table above

        enum class HandOutcome{ Won, Lost, Folded };

        bool playedPreflopThisHand = false;//voluntarily continued preflop (not a free BB check)
        bool firedBluffThisHand = false;//raised without the equity edge to back it
        bool firedValueRaiseThisHand = false;//raised with a genuine equity edge
        bool madeQualifyingCallThisHand = false;//called (or all-in-called-for-less) because margin cleared callBar
        bool foldedThisHand = false;
        bool reachedOwnShowdownThisHand = false;//set by observeShowdown when the shower is self
        bool wonOwnShowdownThisHand = false;

        //MODIFIES: state.parameters
        //EFFECTS: nudges whichever parameters got used this hand based on
        //         how the hand actually turned out - called from
        //         resetForNewHand() before this hand's flags get cleared
        void adapt();
};

class AdvancedCPU: public CPU{
    public:
        //EFFECTS: sets up the underlying player with GTO-adjacent baseline
        //         parameters, lightly jittered so instances aren't
        //         identical - it's buildExploitRead() that actually
        //         differentiates its play from a straight GTO-ish
        //         baseline, not this
        AdvancedCPU(const std::string &nameIn, int chipsIn, int playerIDin);

        //MODIFIES: state.betsFiredThisHand, state.slowplaysThisHand,
        //          state.lastRaiseWasBluff
        //EFFECTS: picks an action - same equity-vs-pot-odds shape as the
        //         other personalities, played close to a GTO-ish baseline
        //         by default, but with call/raise bars and bluff frequency
        //         scaled by buildExploitRead()'s ground-truth read on
        //         whichever opponent is driving this decision. Equity here
        //         is computed the same way as every other personality - a
        //         Monte Carlo estimate against unknown random opponent
        //         cards - this type never looks at another player's actual
        //         hole cards for the hand in progress, only at logged
        //         history from actions that already resolved (see observeAction())
        PlayerAction act(const BasicCPUState& basicState) override;

        //MODIFIES: actionHistory, lastLoggedActionSeq
        //EFFECTS: logs actor's action into the shared cross-player,
        //         cross-hand history, including actor's TRUE win-equity at
        //         the moment (computed from their real hole cards -
        //         Player::getHand() is public, so this isn't a special
        //         privilege, just the only personality that bothers to look
        //         at it). That equity is logged purely as a historical fact
        //         for future reads (buildExploitRead()); it never informs a
        //         decision about the hand actor is currently playing.
        //         stateBeforeAction.actionSeq de-duplicates against getting
        //         notified once per AdvancedCPU instance for the same real
        //         action, since Game::round notifies every player object
        //         (not just this one) after every action. Calls
        //         CPU::observeAction() first for the base raise-tracking
        //         every personality shares
        void observeAction(const Player& actor, const PlayerAction& action,
                            const BasicCPUState& stateBeforeAction) override;

    private:
        //one recorded action from anywhere in the game's history, for any
        //player (not just this bot) - shared (static) across every
        //AdvancedCPU instance, since they all watch the exact same public
        //action stream. equityAtAction is the one thing here no other
        //personality logs: the actor's real win-equity at the time, made
        //possible because Player::getHand() is public and observeAction
        //gets the actor by reference - used only to build a read on
        //opponents' PAST tendencies (buildExploitRead()), never on the hand
        //act() is currently deciding
        struct ActionRecord{
            int playerID;
            BettingStreet street;
            Position position;
            Action action;
            int postActionBet;           //actor's total commitment this round after acting (0 for an early fold)
            int currentHighestBetBefore; //the bet actor faced before acting - together with postActionBet, reconstructs a raise's true size or a call's price
            int potSizeBefore;           //the pot before this action - lets a raise be read as a fraction of the pot, for sizingTellBluffRate()'s sizing tell
            int playersInHandBefore;
            double equityAtAction;       //actor's true win-equity (real hand vs. playersInHandBefore-1 random opponents) at the moment
        };

        //a ground-truth read on one opponent's tendencies at one street,
        //built by aggregating actionHistory - same shape/meaning as
        //BalancedCPU::OpponentRead (1.0 = neutral on all three), just
        //sourced from logged true equity instead of estimated tendencies
        struct ExploitRead{
            double callBarMult = 1.0; //>1: their raises are credible (backed by real equity) - demand more edge to call/raise over them
            double raiseBarMult = 1.0;
            double bluffMult = 1.0;   //>1: they fold under pressure a lot - bluff/apply pressure more
        };

        static std::vector<ActionRecord> actionHistory;
        static long long lastLoggedActionSeq;

        //how many DISTINCT hands (by BasicCPUState::handNumber) each player
        //id has been observed playing, and the last hand number seen for
        //them (to detect a new one) - both indexed by id, grown lazily.
        //This accumulates faster and less noisily than counting some
        //sparser action type (e.g. raise-like actions on one street), which
        //can lag real hands played by several times over - see buildExploitRead()
        static std::vector<int> lastHandSeenForPlayer;
        static std::vector<int> handsSeenForPlayer;

        //EFFECTS: how many distinct hands id has been observed playing, or
        //         0 if id hasn't been observed yet
        static int handsSeenCount(int id);

        //MODIFIES: lastHandSeenForPlayer, handsSeenForPlayer
        //EFFECTS: grows the id-indexed tracking vectors so id is safe to index
        static void ensureHandsSeenTracked(int id);

        //EFFECTS: a ground-truth exploitative read on opponentId at street,
        //         aggregated from actionHistory - defaults to neutral (1.0
        //         on every multiplier, i.e. play the GTO-ish baseline
        //         straight) when opponentId has no relevant history yet
        ExploitRead buildExploitRead(int opponentId, BettingStreet street) const;

        //EFFECTS: a bet of betSize into a pot that was potBeforeBet before
        //         it, as its closed-form game-theoretic "indifference
        //         fraction": alpha = betSize / (potBeforeBet + betSize).
        //         Two standard GTO numbers fall out of this one ratio - as a
        //         bluff-to-value ratio, it's the fraction of a BETTING range
        //         at this size that should be a bluff so a defender is
        //         exactly indifferent between calling and folding; its
        //         complement (1 - alpha) is the Minimum Defense Frequency,
        //         the minimum fraction of a DEFENDING range that must
        //         continue against this bet so it can never be a profitable
        //         bluff no matter what's actually held. Bigger bets push
        //         alpha up (more bluffs per value bet; defenders can afford
        //         to fold more); smaller bets push it down
        static double gtoIndifferenceRatio(double potBeforeBet, double betSize);

        //EFFECTS: opponentId's historical bluff rate specifically among
        //         raise-like actions sized similarly (within
        //         ADVANCED_SIZING_TELL_BAND of betToPotRatio) to the bet
        //         actually being faced THIS decision - a genuine bet-sizing
        //         tell (does this player size bluffs differently from value
        //         bets?) rather than a blanket per-street average. Returns
        //         -1.0 (no signal - not enough matching-sized history to
        //         trust) when fewer than ADVANCED_SIZING_TELL_MIN_SAMPLES qualify
        double sizingTellBluffRate(int opponentId, double betToPotRatio) const;

        //EFFECTS: the probability of taking the more aggressive side of a
        //         decision (raising instead of checking/folding, calling
        //         instead of folding, continuing preflop instead of folding
        //         to the starting-hand screen) given how far margin sits
        //         above or below bar. A hard deterministic cutoff has an
        //         exact, learnable line an opponent could eventually pick
        //         up on - a genuine Nash equilibrium only stays
        //         unexploitable if hands right at the indifference point
        //         mix between actions instead, exactly what solved toy
        //         games (Kuhn poker, the AKQ game) show: the "middle" of a
        //         range plays a specific MIXED frequency, not one
        //         deterministic action. Well above/below bar this saturates
        //         to ~1.0/~0.0, so only genuinely close decisions get
        //         materially randomized
        static double mixedStrategyProbability(double margin, double bar, double width);
};

//EFFECTS: builds a player of the given CPU type, name, buy-in, and id
std::unique_ptr<Player> createCPUPlayer(PlayerType type, const std::string &name,
                                        int buyIn, int id);
