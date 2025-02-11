#ifndef NEURALNET_NNINPUTS_H_
#define NEURALNET_NNINPUTS_H_

#include <memory>

#include "../core/global.h"
#include "../core/hash.h"
#include "../core/rand.h"
#include "../game/board.h"
#include "../game/boardhistory.h"

namespace NNPos {
  constexpr int MAX_BOARD_LEN = Board::MAX_LEN;
  constexpr int MAX_BOARD_AREA = MAX_BOARD_LEN * MAX_BOARD_LEN;
  // Policy output adds +1 for the pass move
  constexpr int MAX_NN_POLICY_SIZE = MAX_BOARD_AREA * NUM_ACTUAL_DIRECTIONS;
  // Extra score distribution radius, used for writing score in data rows and for the neural net score belief output
  constexpr int EXTRA_SCORE_DISTR_RADIUS = 60;
  // Used various places we clip komi beyond board area.
  constexpr float KOMI_CLIP_RADIUS = 20.0f;

  int xydToPos(int x, int y, Direction dir, int nnXLen, int nnYLen);
  int locToPos(Loc loc, int boardXSize, int nnXLen, int nnYLen);
  Loc posToLoc(int pos, int boardXSize, int boardYSize, int nnXLen, int nnYLen);
  int xyToPos(int x, int y, int nnXLen);
  int spotToPos(Spot spot, int boardXSize, int nnXLen, int nnYLen);
  Spot PosToSpot(int pos, int boardXSize, int boardYSize, int nnXLen, int nnYLen);
  int getPolicySize(int nnXLen, int nnYLen);
}  // namespace NNPos

namespace NNInputs {
  constexpr int SYMMETRY_NOTSPECIFIED = -1;
  constexpr int SYMMETRY_ALL = -2;
}  // namespace NNInputs

struct MiscNNInputParams {
  double playoutDoublingAdvantage = 0.0;
  float nnPolicyTemperature = 1.0f;
  // If no symmetry is specified, it will use default or random based on config, unless node is already cached.
  int symmetry = NNInputs::SYMMETRY_NOTSPECIFIED;
  double policyOptimism = 0.0;

  static const Hash128 ZOBRIST_PLAYOUT_DOUBLINGS;
  static const Hash128 ZOBRIST_NN_POLICY_TEMP;
  static const Hash128 ZOBRIST_POLICY_OPTIMISM;
};

namespace NNInputs {
  const bool historyChannelWithDirection;  // for test, remove later, set it in nninputs.cpp
  const int NUM_FEATURES_SPATIAL_V1 = 16;
  const int NUM_FEATURES_GLOBAL_V1 = 1;

  Hash128 getHash(
    const Board& board,
    const BoardHistory& boardHistory,
    Player nextPlayer,
    const MiscNNInputParams& nnInputParams);

  void fillRowV1(
    const Board& board,
    const BoardHistory& boardHistory,
    Player nextPlayer,
    const MiscNNInputParams& nnInputParams,
    int nnXLen,
    int nnYLen,
    bool useNHWC,
    float* rowBin,
    float* rowGlobal);

  // If groupTax is specified, for each color region of area, reduce weight on empty spaces equally to reduce the total
  // sum by 2. (but should handle seki correctly)
  void fillScoring(const Board& board, const Color* area, bool groupTax, float* scoring);
}  // namespace NNInputs

struct NNOutput {
  Hash128 nnHash;  // NNInputs - getHash

  // Initially from the perspective of the player to move at the time of the eval, fixed up later in nnEval.cpp
  // to be the value from white's perspective.
  // These three are categorial probabilities for each outcome.
  float whiteWinProb;
  float whiteLossProb;

  // Expected arrival time of remaining game variance, in turns, weighted by variance, only when modelVersion >= 9
  float varTimeLeft;
  // A metric indicating the "typical" error in the winloss value or the score that the net expects, relative to the
  // short-term future MCTS value.
  float shorttermWinlossError;

  // Indexed by pos rather than loc
  // Values in here will be set to negative for illegal moves, including superko
  float policyProbs[NNPos::MAX_NN_POLICY_SIZE];

  int nnXLen;
  int nnYLen;
  // If not NULL, then this contains a nnXLen*nnYLen-sized map of expected ownership on the board.
  float* whiteOwnerMap;

  // If not NULL, then contains policy with dirichlet noise or any other noise adjustments for this node
  float* noisedPolicyProbs;

  NNOutput();  // Does NOT initialize values
  NNOutput(const NNOutput& other);
  ~NNOutput();

  // Averages the others. Others must be nonempty and share the same nnHash (i.e. be for the same board, except if
  // somehow the hash collides) Does NOT carry over noisedPolicyProbs.
  NNOutput(const std::vector<std::shared_ptr<NNOutput>>& others);

  NNOutput& operator=(const NNOutput&);

  inline float* getPolicyProbsMaybeNoised() { return noisedPolicyProbs != NULL ? noisedPolicyProbs : policyProbs; }
  inline const float* getPolicyProbsMaybeNoised() const {
    return noisedPolicyProbs != NULL ? noisedPolicyProbs : policyProbs;
  }
  void debugPrint(std::ostream& out, const Board& board);
  inline int getPos(Loc loc, const Board& board) const { return NNPos::locToPos(loc, board.x_size, nnXLen, nnYLen); }
};

namespace SymmetryHelpers {
  // A symmetry is 3 bits flipY(bit 0), flipX(bit 1), transpose(bit 2). They are applied in that order.
  // The first four symmetries only reflect, and do not transpose X and Y.
  constexpr int NUM_SYMMETRIES = 8;
  constexpr int NUM_SYMMETRIES_WITHOUT_TRANSPOSE = 4;

  // These two IGNORE transpose if hSize and wSize do not match. So non-square transposes are disallowed.
  // copyOutputsWithSymmetry performs the inverse of symmetry.
  void copyInputsWithSymmetry(
    const float* src,
    float* dst,
    int nSize,
    int hSize,
    int wSize,
    int cSize,
    bool useNHWC,
    int symmetry);
  void copyOutputsWithSymmetry(const float* src, float* dst, int nSize, int hSize, int wSize, int symmetry);

  // Applies a symmetry to a location
  Spot getSymSpot(int x, int y, const Board& board, int symmetry);
  Spot getSymSpot(Spot spot, const Board& board, int symmetry);
  Spot getSymSpot(int x, int y, int xSize, int ySize, int symmetry);
  Spot getSymSpot(Spot spot, int xSize, int ySize, int symmetry);
  Direction getSymDir(Direction dir, int symmetry);
  // Applies a symmetry to a board
  Board getSymBoard(const Board& board, int symmetry);

  // Get the inverse of the specified symmetry
  int invert(int symmetry);
  // Get the symmetry equivalent to first applying firstSymmetry and then applying nextSymmetry.
  int compose(int firstSymmetry, int nextSymmetry);
  int compose(int firstSymmetry, int nextSymmetry, int nextNextSymmetry);

  inline bool isTranspose(int symmetry) {
    return (symmetry & 0x4) != 0;
  }
  inline bool isFlipX(int symmetry) {
    return (symmetry & 0x2) != 0;
  }
  inline bool isFlipY(int symmetry) {
    return (symmetry & 0x1) != 0;
  }

  // Fill isSymDupLoc with true on all but one copy of each symmetrically equivalent move, and false everywhere else.
  // isSymDupLocs should be an array of size Board::MAX_ARR_SIZE
  // If onlySymmetries is not NULL, will only consider the symmetries specified there.
  // validSymmetries will be filled with all symmetries of the current board, including using history for checking
  // ko/superko and some encore-related state. This implementation is dependent on specific order of the symmetries
  // (i.e. transpose is coded as 0x4) Will pretend moves that have a nonzero value in avoidMoves do not exist.
  void markDuplicateMoveLocs(
    const Board& board,
    const BoardHistory& hist,
    const std::vector<int>* onlySymmetries,
    const std::vector<int>& avoidMoves,
    bool* isSymDupLoc,
    std::vector<int>& validSymmetries);

  // For each symmetry, return a metric about the "amount" of difference that board would have with other
  // if symmetry were applied to board.
  void getSymmetryDifferences(
    const Board& board,
    const Board& other,
    double maxDifferenceToReport,
    double symmetryDifferences[NUM_SYMMETRIES]);
}  // namespace SymmetryHelpers

// Utility functions for computing the "scoreValue", the unscaled utility of various numbers of points, prior to
// multiplication by staticScoreUtilityFactor or dynamicScoreUtilityFactor (see searchparams.h)
namespace ScoreValue {
  // MUST BE CALLED AT PROGRAM START!
  void initTables();
  void freeTables();

  // The number of wins a game result should count as
  double whiteWinsOfWinner(Player winner);

  // The unscaled utility of achieving a certain score difference
  double whiteScoreValueOfScoreSmooth(
    double finalWhiteMinusBlackScore,
    double center,
    double scale,
    double drawEquivalentWinsForWhite,
    const Board& b,
    const BoardHistory& hist);
  double whiteScoreValueOfScoreSmoothNoDrawAdjust(
    double finalWhiteMinusBlackScore,
    double center,
    double scale,
    const Board& b);
  // Approximately invert whiteScoreValueOfScoreSmooth
  double approxWhiteScoreOfScoreValueSmooth(double scoreValue, double center, double scale, const Board& b);
  // The derviative of whiteScoreValueOfScoreSmoothNoDrawAdjust with respect to finalWhiteMinusBlackScore.
  double whiteDScoreValueDScoreSmoothNoDrawAdjust(
    double finalWhiteMinusBlackScore,
    double center,
    double scale,
    const Board& b);

  // Compute what the scoreMeanSq should be for a final game result
  // It is NOT simply the same as finalWhiteMinusBlackScore^2 because for integer komi we model it as a distribution
  // where with the appropriate probability you gain or lose 0.5 point to achieve the desired
  // drawEquivalentWinsForWhite, so it actually has some variance.
  double whiteScoreMeanSqOfScoreGridded(double finalWhiteMinusBlackScore, double drawEquivalentWinsForWhite);

  // The expected unscaled utility of the final score difference, given the mean and stdev of the distribution of that
  // difference, assuming roughly a normal distribution.
  double
  expectedWhiteScoreValue(double whiteScoreMean, double whiteScoreStdev, double center, double scale, const Board& b);

  // Get the standard deviation of score given the E(score) and E(score^2)
  double getScoreStdev(double scoreMeanAvg, double scoreMeanSqAvg);
}  // namespace ScoreValue

#endif  // NEURALNET_NNINPUTS_H_
