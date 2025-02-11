#ifndef PROGRAM_PLAY_H_
#define PROGRAM_PLAY_H_

#include "../core/config_parser.h"
#include "../core/global.h"
#include "../core/multithread.h"
#include "../core/rand.h"
#include "../core/threadsafecounter.h"
#include "../core/threadsafequeue.h"
#include "../dataio/sgf.h"
#include "../dataio/trainingwrite.h"
#include "../game/board.h"
#include "../game/boardhistory.h"
#include "../program/playsettings.h"
#include "../search/search.h"
#include "../search/searchparams.h"

struct InitialPosition {
  Board board;
  BoardHistory hist;
  Player pla;
  bool isPlainFork;
  bool isHintFork;
  double trainingWeight;

  InitialPosition();
  InitialPosition(
    const Board& board,
    const BoardHistory& hist,
    Player pla,
    bool isPlainFork,
    bool isHintFork,
    double trainingWeight);
  ~InitialPosition();
};

// Holds various initial positions that we may start from rather than a whole new game
struct ForkData {
  std::mutex mutex;
  std::vector<const InitialPosition*> forks;
  std::vector<const InitialPosition*> sekiForks;
  ~ForkData();

  void add(const InitialPosition* pos);
  const InitialPosition* get(Rand& rand);

  void addSeki(const InitialPosition* pos, Rand& rand);
  const InitialPosition* getSeki(Rand& rand);
};

struct OtherGameProperties {
  bool isSgfPos = false;
  bool isHintPos = false;
  bool allowPolicyInit = true;
  bool isFork = false;
  bool isHintFork = false;

  int hintTurn = -1;
  Hash128 hintPosHash;
  Loc hintLoc = Loc(Board::NULL_LOC, D_NONE);

  // Note: these two behave slightly differently than the ones in searchParams - as properties for the whole
  // game, they make the playouts *actually* vary instead of only making the neural net think they do.
  double playoutDoublingAdvantage = 0.0;
  Player playoutDoublingAdvantagePla = C_EMPTY;
};

// Object choosing random initial rules and board sizes for games. Threadsafe.
class GameInitializer {
 public:
  GameInitializer(ConfigParser& cfg, Logger& logger);
  GameInitializer(ConfigParser& cfg, Logger& logger, const std::string& randSeed);
  ~GameInitializer();

  GameInitializer(const GameInitializer&) = delete;
  GameInitializer& operator=(const GameInitializer&) = delete;

  // Initialize everything for a new game with random rules, unless initialPosition is provided, in which case it uses
  // those rules (possibly with noise to the komi given in that position)
  // Also, mutates params to randomize appropriate things like utilities, but does NOT fill in all the settings.
  // User should make sure the initial params provided makes sense as a mean or baseline.
  // Does NOT place handicap stones, users of this function need to place them manually
  void createGame(
    Board& board,
    Player& pla,
    BoardHistory& hist,
    SearchParams& params,
    const InitialPosition* initialPosition,
    const PlaySettings& playSettings,
    OtherGameProperties& otherGameProps,
    const Sgf::PositionSample* startPosSample);

  // A version that doesn't randomize params
  void createGame(
    Board& board,
    Player& pla,
    BoardHistory& hist,
    const InitialPosition* initialPosition,
    const PlaySettings& playSettings,
    OtherGameProperties& otherGameProps,
    const Sgf::PositionSample* startPosSample);

  bool isAllowedBSize(int xSize, int ySize);

  std::vector<std::pair<int, int>> getAllowedBSizes() const;
  int getMinBoardXSize() const;
  int getMinBoardYSize() const;
  int getMaxBoardXSize() const;
  int getMaxBoardYSize() const;

 private:
  void initShared(ConfigParser& cfg, Logger& logger);
  void createGameSharedUnsynchronized(
    Board& board,
    Player& pla,
    BoardHistory& hist,
    const InitialPosition* initialPosition,
    const PlaySettings& playSettings,
    OtherGameProperties& otherGameProps,
    const Sgf::PositionSample* startPosSample);

  std::mutex createGameMutex;
  Rand rand;

  std::vector<std::pair<int, int>> allowedBSizes;
  std::vector<double> allowedBSizeRelProbs;

  double allowRectangleProb;

  std::vector<Sgf::PositionSample> startPoses;
  std::vector<double> startPosCumProbs;
  double startPosesProb;

  std::vector<Sgf::PositionSample> hintPoses;
  std::vector<double> hintPosCumProbs;
  double hintPosesProb;

  int minBoardXSize;
  int minBoardYSize;
  int maxBoardXSize;
  int maxBoardYSize;
};

// Object for generating and servering evenly distributed pairings between different bots. Threadsafe.
class MatchPairer {
 public:
  // Holds pointers to the various nnEvals, but does NOT take ownership for freeing them.
  MatchPairer(
    ConfigParser& cfg,
    int numBots,
    const std::vector<std::string>& botNames,
    const std::vector<NNEvaluator*>& nnEvals,
    const std::vector<SearchParams>& baseParamss,
    const std::vector<std::pair<int, int>>& matchupsPerRound,
    int64_t numGamesTotal);

  ~MatchPairer();

  struct BotSpec {
    int botIdx;
    std::string botName;
    NNEvaluator* nnEval;
    SearchParams baseParams;
  };

  MatchPairer(const MatchPairer&) = delete;
  MatchPairer& operator=(const MatchPairer&) = delete;

  // Get the total number of games that the matchpairer will generate
  int64_t getNumGamesTotalToGenerate() const;

  // Get next matchup and log stuff
  bool getMatchup(BotSpec& botSpecB, BotSpec& botSpecW, Logger& logger);

 private:
  const int numBots;
  const std::vector<std::string> botNames;
  const std::vector<NNEvaluator*> nnEvals;
  const std::vector<SearchParams> baseParamss;
  const std::vector<std::pair<int, int>> matchupsPerRound;

  std::vector<std::pair<int, int>> nextMatchups;
  Rand rand;

  int64_t numGamesStartedSoFar;
  const int64_t numGamesTotal;
  int64_t logGamesEvery;

  std::mutex getMatchupMutex;

  std::pair<int, int> getMatchupPairUnsynchronized();
};

// Functions to run a single game or other things
namespace Play {

  // In the case where checkForNewNNEval is provided, will MODIFY the provided botSpecs with any new nneval!
  // onEachMove is actually not used (as of KataGo 1.13.2)
  FinishedGameData* runGame(
    const Board& startBoard,
    Player pla,
    const BoardHistory& startHist,
    MatchPairer::BotSpec& botSpecB,
    MatchPairer::BotSpec& botSpecW,
    const std::string& searchRandSeed,
    bool clearBotBeforeSearch,
    Logger& logger,
    bool logSearchInfo,
    bool logMoves,
    int maxMovesPerGame,
    const std::function<bool()>& shouldStop,
    const WaitableFlag* shouldPause,
    const PlaySettings& playSettings,
    const OtherGameProperties& otherGameProps,
    Rand& gameRand,
    std::function<NNEvaluator*()> checkForNewNNEval,
    std::function<void(
      const Board&,
      const BoardHistory&,
      Player,
      Loc,
      const std::vector<double>&,
      const std::vector<double>&,
      const std::vector<double>&,
      const Search*)> onEachMove);

  // In the case where checkForNewNNEval is provided, will MODIFY the provided botSpecs with any new nneval!
  FinishedGameData* runGame(
    const Board& startBoard,
    Player pla,
    const BoardHistory& startHist,
    MatchPairer::BotSpec& botSpecB,
    MatchPairer::BotSpec& botSpecW,
    Search* botB,
    Search* botW,
    bool clearBotBeforeSearch,
    Logger& logger,
    bool logSearchInfo,
    bool logMoves,
    int maxMovesPerGame,
    const std::function<bool()>& shouldStop,
    const WaitableFlag* shouldPause,
    const PlaySettings& playSettings,
    const OtherGameProperties& otherGameProps,
    Rand& gameRand,
    std::function<NNEvaluator*()> checkForNewNNEval,
    std::function<void(
      const Board&,
      const BoardHistory&,
      Player,
      Loc,
      const std::vector<double>&,
      const std::vector<double>&,
      const std::vector<double>&,
      const Search*)> onEachMove);

  void maybeForkGame(
    const FinishedGameData* finishedGameData,
    ForkData* forkData,
    const PlaySettings& playSettings,
    Rand& gameRand,
    Search* bot);

  void maybeHintForkGame(
    const FinishedGameData* finishedGameData,
    ForkData* forkData,
    const OtherGameProperties& otherGameProps);

}  // namespace Play

// Class for running a game and enqueueing the result as training data.
// Wraps together most of the neural-net-independent parameters to spawn and run a full game.
class GameRunner {
  bool logSearchInfo;
  bool logMoves;
  int maxMovesPerGame;
  bool clearBotBeforeSearch;
  PlaySettings playSettings;
  GameInitializer* gameInit;

 public:
  GameRunner(ConfigParser& cfg, PlaySettings playSettings, Logger& logger);
  GameRunner(ConfigParser& cfg, const std::string& gameInitRandSeed, PlaySettings fModes, Logger& logger);
  ~GameRunner();

  // Will return NULL if stopped before the game completes. The caller is responsible for freeing the data
  // if it isn't NULL.
  // afterInitialization can be used to run any post-initialization configuration on the search
  FinishedGameData* runGame(
    const std::string& seed,
    const MatchPairer::BotSpec& botSpecB,
    const MatchPairer::BotSpec& botSpecW,
    ForkData* forkData,
    const Sgf::PositionSample* startPosSample,
    Logger& logger,
    const std::function<bool()>& shouldStop,
    const WaitableFlag* shouldPause,
    std::function<NNEvaluator*()> checkForNewNNEval,
    std::function<void(const MatchPairer::BotSpec&, Search*)> afterInitialization,
    std::function<void(
      const Board&,
      const BoardHistory&,
      Player,
      Loc,
      const std::vector<double>&,
      const std::vector<double>&,
      const std::vector<double>&,
      const Search*)> onEachMove);

  const GameInitializer* getGameInitializer() const;
};

#endif  // PROGRAM_PLAY_H_
