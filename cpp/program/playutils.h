#ifndef PROGRAM_PLAY_UTILS_H_
#define PROGRAM_PLAY_UTILS_H_

#include "../core/config_parser.h"
#include "../program/play.h"
#include "../search/asyncbot.h"

// This is a grab-bag of various useful higher-level functions that select moves or evaluate the board in various ways.

namespace PlayUtils {
  // Use the given bot to play free handicap stones, modifying the board and hist in the process and setting the bot's
  // position to it. Does NOT switch the initial player of the board history to white
  void
  playExtraBlack(Search* bot, int numExtraBlack, Board& board, BoardHistory& hist, double temperature, Rand& gameRand);

  // Set board to empty and place fixed handicap stones, raising an exception if invalid
  void placeFixedHandicap(Board& board, int n);

  ReportedSearchValues getWhiteScoreValues(
    Search* bot,
    const Board& board,
    const BoardHistory& hist,
    Player pla,
    int64_t numVisits,
    const OtherGameProperties& otherGameProps);

  Loc chooseRandomLegalMove(const Board& board, const BoardHistory& hist, Player pla, Rand& gameRand, Loc banMove);
  int chooseRandomLegalMoves(
    const Board& board,
    const BoardHistory& hist,
    Player pla,
    Rand& gameRand,
    Loc* buf,
    int len);

  Loc chooseRandomPolicyMove(
    const NNOutput* nnOutput,
    const Board& board,
    const BoardHistory& hist,
    Player pla,
    Rand& gameRand,
    double temperature,
    bool allowPass,
    Loc banMove);

  Loc getGameInitializationMove(
    Search* botB,
    Search* botW,
    Board& board,
    const BoardHistory& hist,
    Player pla,
    NNResultBuf& buf,
    Rand& gameRand,
    double temperature);
  void initializeGameUsingPolicy(
    Search* botB,
    Search* botW,
    Board& board,
    BoardHistory& hist,
    Player& pla,
    Rand& gameRand,
    double proportionOfBoardArea,
    double temperature);

  double getSearchFactor(
    double searchFactorWhenWinningThreshold,
    double searchFactorWhenWinning,
    const SearchParams& params,
    const std::vector<double>& recentWinLossValues,
    Player pla);

  std::vector<double>
  computeOwnership(Search* bot, const Board& board, const BoardHistory& hist, Player pla, int64_t numVisits);

  // Determine all living and dead stones, if the game were terminated right now and
  // the rules were interpreted naively and directly.
  // Returns a vector indexed by board Loc (length Board::MAX_ARR_SIZE).
  std::vector<bool> computeAnticipatedStatusesSimple(const Board& board, const BoardHistory& hist);

  // Determine all living and dead stones, trying to be clever and use the ownership prediction
  // of the neural net.
  // Returns a vector indexed by board Loc (length Board::MAX_ARR_SIZE).
  std::vector<bool> computeAnticipatedStatusesWithOwnership(
    Search* bot,
    const Board& board,
    const BoardHistory& hist,
    Player pla,
    int64_t numVisits,
    std::vector<double>& ownershipsBuf);

  struct BenchmarkResults {
    int numThreads = 0;
    int totalPositionsSearched = 0;
    int totalPositions = 0;
    int64_t totalVisits = 0;
    double totalSeconds = 0;
    int64_t numNNEvals = 0;
    int64_t numNNBatches = 0;
    double avgBatchSize = 0;

    std::string toStringNotDone() const;
    std::string toString() const;
    std::string toStringWithElo(const BenchmarkResults* baseline, double secondsPerGameMove) const;

    double computeEloEffect(double secondsPerGameMove) const;

    static void printEloComparison(const std::vector<BenchmarkResults>& results, double secondsPerGameMove);
  };

  // Run benchmark on sgf positions. ALSO prints to stdout the ongoing result as it benchmarks.
  BenchmarkResults benchmarkSearchOnPositionsAndPrint(
    const SearchParams& params,
    const CompactSgf* sgf,
    int numPositionsToUse,
    NNEvaluator* nnEval,
    const BenchmarkResults* baseline,
    double secondsPerGameMove,
    bool printElo);

  void printGenmoveLog(
    std::ostream& out,
    const AsyncBot* bot,
    const NNEvaluator* nnEval,
    double timeTaken,
    Player perspective);

  std::shared_ptr<NNOutput> getFullSymmetryNNOutput(
    const Board& board,
    const BoardHistory& hist,
    Player pla,
    bool includeOwnerMap,
    NNEvaluator* nnEval);

}  // namespace PlayUtils

#endif  // PROGRAM_PLAY_UTILS_H_
