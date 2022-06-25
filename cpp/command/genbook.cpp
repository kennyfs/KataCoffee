#include "../core/global.h"
#include "../core/makedir.h"
#include "../core/config_parser.h"
#include "../core/fileutils.h"
#include "../core/timer.h"
#include "../core/threadsafequeue.h"
#include "../dataio/sgf.h"
#include "../dataio/files.h"
#include "../book/book.h"
#include "../search/searchnode.h"
#include "../search/asyncbot.h"
#include "../program/setup.h"
#include "../program/playutils.h"
#include "../program/play.h"
#include "../command/commandline.h"
#include "../main.h"

#include <chrono>
#include <csignal>

using namespace std;

static std::atomic<bool> sigReceived(false);
static std::atomic<bool> shouldStop(false);
static void signalHandler(int signal)
{
  if(signal == SIGINT || signal == SIGTERM) {
    sigReceived.store(true);
    shouldStop.store(true);
  }
}

static double getMaxPolicy(float policyProbs[NNPos::MAX_NN_POLICY_SIZE]) {
  double maxPolicy = 0.0;
  for(int i = 0; i<NNPos::MAX_NN_POLICY_SIZE; i++)
    if(policyProbs[i] > maxPolicy)
      maxPolicy = policyProbs[i];
  return maxPolicy;
}


int MainCmds::genbook(const vector<string>& args) {
  Board::initHash();
  ScoreValue::initTables();

  ConfigParser cfg;
  string modelFile;
  string htmlDir;
  string bookFile;
  string traceBookFile;
  string logFile;
  string bonusFile;
  int numIterations;
  int saveEveryIterations;
  double traceBookMinVisits;
  bool allowChangingBookParams;
  bool htmlDevMode;
  double htmlMinVisits;
  try {
    KataGoCommandLine cmd("Generate opening book");
    cmd.addConfigFileArg("","",true);
    cmd.addModelFileArg();
    cmd.addOverrideConfigArg();

    TCLAP::ValueArg<string> htmlDirArg("","html-dir","HTML directory to export to, at the end of -num-iters",false,string(),"DIR");
    TCLAP::ValueArg<string> bookFileArg("","book-file","Book file to write to or continue expanding",true,string(),"FILE");
    TCLAP::ValueArg<string> traceBookFileArg("","trace-book-file","Other book file we should copy all the lines from",false,string(),"FILE");
    TCLAP::ValueArg<string> logFileArg("","log-file","Log file to write to",true,string(),"DIR");
    TCLAP::ValueArg<string> bonusFileArg("","bonus-file","SGF of bonuses marked",false,string(),"DIR");
    TCLAP::ValueArg<int> numIterationsArg("","num-iters","Number of iterations to expand book",true,0,"N");
    TCLAP::ValueArg<int> saveEveryIterationsArg("","save-every","Number of iterations per save to book file",true,0,"N");
    TCLAP::ValueArg<double> traceBookMinVisitsArg("","trace-book-min-visits","Require >= this many visits for copying from traceBookFile",false,0.0,"N");
    TCLAP::SwitchArg allowChangingBookParamsArg("","allow-changing-book-params","Allow changing book params");
    TCLAP::SwitchArg htmlDevModeArg("","html-dev-mode","Denser debug output for html");
    TCLAP::ValueArg<double> htmlMinVisitsArg("","html-min-visits","Require >= this many visits to export a position to html",false,0.0,"N");
    cmd.add(htmlDirArg);
    cmd.add(bookFileArg);
    cmd.add(traceBookFileArg);
    cmd.add(logFileArg);
    cmd.add(bonusFileArg);
    cmd.add(numIterationsArg);
    cmd.add(saveEveryIterationsArg);
    cmd.add(traceBookMinVisitsArg);
    cmd.add(allowChangingBookParamsArg);
    cmd.add(htmlDevModeArg);
    cmd.add(htmlMinVisitsArg);

    cmd.parseArgs(args);

    cmd.getConfig(cfg);
    modelFile = cmd.getModelFile();
    htmlDir = htmlDirArg.getValue();
    bookFile = bookFileArg.getValue();
    traceBookFile = traceBookFileArg.getValue();
    logFile = logFileArg.getValue();
    bonusFile = bonusFileArg.getValue();
    numIterations = numIterationsArg.getValue();
    saveEveryIterations = saveEveryIterationsArg.getValue();
    traceBookMinVisits = traceBookMinVisitsArg.getValue();
    allowChangingBookParams = allowChangingBookParamsArg.getValue();
    htmlDevMode = htmlDevModeArg.getValue();
    htmlMinVisits = htmlMinVisitsArg.getValue();
  }
  catch (TCLAP::ArgException &e) {
    cerr << "Error: " << e.error() << " for argument " << e.argId() << endl;
    return 1;
  }

  Rand rand;
  const bool logToStdoutDefault = true;
  Logger logger(&cfg, logToStdoutDefault);
  logger.addFile(logFile);

  const bool loadKomiFromCfg = true;
  Rules rules = Setup::loadSingleRules(cfg,loadKomiFromCfg);

  const int boardSizeX = cfg.getInt("boardSizeX",2,Board::MAX_LEN);
  const int boardSizeY = cfg.getInt("boardSizeY",2,Board::MAX_LEN);
  const int repBound = cfg.getInt("repBound",3,1000);
  const double errorFactor = cfg.getDouble("errorFactor",0.01,100.0);
  const double costPerMove = cfg.getDouble("costPerMove",0.0,1000000.0);
  const double costPerUCBWinLossLoss = cfg.getDouble("costPerUCBWinLossLoss",0.0,1000000.0);
  const double costPerUCBWinLossLossPow3 = cfg.getDouble("costPerUCBWinLossLossPow3",0.0,1000000.0);
  const double costPerUCBWinLossLossPow7 = cfg.getDouble("costPerUCBWinLossLossPow7",0.0,1000000.0);
  const double costPerUCBScoreLoss = cfg.getDouble("costPerUCBScoreLoss",0.0,1000000.0);
  const double costPerLogPolicy = cfg.getDouble("costPerLogPolicy",0.0,1000000.0);
  const double costPerMovesExpanded = cfg.getDouble("costPerMovesExpanded",0.0,1000000.0);
  const double costPerSquaredMovesExpanded = cfg.getDouble("costPerSquaredMovesExpanded",0.0,1000000.0);
  const double costWhenPassFavored = cfg.getDouble("costWhenPassFavored",0.0,1000000.0);
  const double bonusPerWinLossError = cfg.getDouble("bonusPerWinLossError",0.0,1000000.0);
  const double bonusPerScoreError = cfg.getDouble("bonusPerScoreError",0.0,1000000.0);
  const double bonusPerSharpScoreDiscrepancy = cfg.getDouble("bonusPerSharpScoreDiscrepancy",0.0,1000000.0);
  const double bonusPerExcessUnexpandedPolicy = cfg.getDouble("bonusPerExcessUnexpandedPolicy",0.0,1000000.0);
  const double bonusForWLPV1 = cfg.contains("bonusForWLPV1") ? cfg.getDouble("bonusForWLPV1",0.0,1000000.0) : 0.0;
  const double bonusForWLPV2 = cfg.contains("bonusForWLPV2") ? cfg.getDouble("bonusForWLPV2",0.0,1000000.0) : 0.0;
  const double bonusForBiggestWLCost = cfg.contains("bonusForBiggestWLCost") ? cfg.getDouble("bonusForBiggestWLCost",0.0,1000000.0) : 0.0;
  const double scoreLossCap = cfg.getDouble("scoreLossCap",0.0,1000000.0);
  const double utilityPerScore = cfg.getDouble("utilityPerScore",0.0,1000000.0);
  const double policyBoostSoftUtilityScale = cfg.getDouble("policyBoostSoftUtilityScale",0.0,1000000.0);
  const double utilityPerPolicyForSorting = cfg.getDouble("utilityPerPolicyForSorting",0.0,1000000.0);
  const double maxVisitsForReExpansion = cfg.contains("maxVisitsForReExpansion") ? cfg.getDouble("maxVisitsForReExpansion",0.0,1e50) : 0.0;
  const double sharpScoreOutlierCap = cfg.getDouble("sharpScoreOutlierCap",0.0,1000000.0);
  const bool logSearchInfo = cfg.getBool("logSearchInfo");
  const string rulesLabel = cfg.getString("rulesLabel");
  const string rulesLink = cfg.getString("rulesLink");

  const int64_t minTreeVisitsToRecord = cfg.getInt64("minTreeVisitsToRecord", (int64_t)1, (int64_t)1 << 50);
  const int maxDepthToRecord = cfg.getInt("maxDepthToRecord", 1, 100);
  const int64_t maxVisitsForLeaves = cfg.getInt64("maxVisitsForLeaves", (int64_t)1, (int64_t)1 << 50);

  const int numGameThreads = cfg.getInt("numGameThreads",1,1000);
  const int numToExpandPerIteration = cfg.getInt("numToExpandPerIteration",1,10000000);

  std::map<BookHash,double> bonusByHash;
  Board bonusInitialBoard(boardSizeX,boardSizeY);
  Player bonusInitialPla = P_BLACK;
  if(bonusFile != "") {
    Sgf* sgf = Sgf::loadFile(bonusFile);
    std::set<Hash128> uniqueHashes;
    bool hashComments = true;
    bool hashParent = true;
    bool flipIfPassOrWFirst = false;
    bool allowGameOver = false;
    Rand seedRand("bonusByHash");
    sgf->iterAllUniquePositions(
      uniqueHashes, hashComments, hashParent, flipIfPassOrWFirst, allowGameOver, &seedRand,
      [&](Sgf::PositionSample& unusedSample, const BoardHistory& sgfHist, const string& comments) {
        (void)unusedSample;
        if(comments.size() > 0 && comments.find("BONUS") != string::npos) {
          BoardHistory hist(sgfHist.initialBoard, sgfHist.initialPla, rules, sgfHist.initialEncorePhase);
          Board board = hist.initialBoard;
          for(size_t i = 0; i<sgfHist.moveHistory.size(); i++) {
            bool suc = hist.makeBoardMoveTolerant(board, sgfHist.moveHistory[i].loc, sgfHist.moveHistory[i].pla);
            if(!suc)
              return;
          }
          BookHash hashRet;
          int symmetryToAlignRet;
          vector<int> symmetriesRet;

          double bonus = Global::stringToDouble(Global::trim(comments.substr(comments.find("BONUS")+5)));
          for(int bookVersion = 1; bookVersion < Book::LATEST_BOOK_VERSION; bookVersion++) {
            BookHash::getHashAndSymmetry(hist, repBound, hashRet, symmetryToAlignRet, symmetriesRet, bookVersion);
            bonusByHash[hashRet] = bonus;
            logger.write("Adding bonus " + Global::doubleToString(bonus) + " to hash " + hashRet.toString());
          }
        }
      }
    );

    XYSize xySize = sgf->getXYSize();
    if(boardSizeX != xySize.x || boardSizeY != xySize.y)
      throw StringError("Board size in config does not match the board size of the bonus file");
    vector<Move> placements;
    sgf->getPlacements(placements,boardSizeX,boardSizeY);
    bool suc = bonusInitialBoard.setStonesFailIfNoLibs(placements);
    if(!suc)
      throw StringError("Invalid placements in sgf");
    bonusInitialPla = sgf->getFirstPlayerColor();
  }

  const SearchParams params = Setup::loadSingleParams(cfg,Setup::SETUP_FOR_GTP);
  const double wideRootNoiseBookExplore = cfg.contains("wideRootNoiseBookExplore") ? cfg.getDouble("wideRootNoiseBookExplore",0.0,5.0) : params.wideRootNoise;
  const double cpuctExplorationLogBookExplore = cfg.contains("cpuctExplorationLogBookExplore") ? cfg.getDouble("cpuctExplorationLogBookExplore",0.0,10.0) : params.cpuctExplorationLog;
  NNEvaluator* nnEval;
  {
    Setup::initializeSession(cfg);
    const int maxConcurrentEvals = numGameThreads * params.numThreads * 2 + 16; // * 2 + 16 just to give plenty of headroom
    const int expectedConcurrentEvals = numGameThreads * params.numThreads;
    const int defaultMaxBatchSize = std::max(8,((numGameThreads * params.numThreads+3)/4)*4);
    const bool defaultRequireExactNNLen = true;
    const bool disableFP16 = false;
    const string expectedSha256 = "";
    nnEval = Setup::initializeNNEvaluator(
      modelFile,modelFile,expectedSha256,cfg,logger,rand,maxConcurrentEvals,expectedConcurrentEvals,
      boardSizeX,boardSizeY,defaultMaxBatchSize,defaultRequireExactNNLen,disableFP16,
      Setup::SETUP_FOR_ANALYSIS
    );
  }
  logger.write("Loaded neural net");

  vector<Search*> searches;
  for(int i = 0; i<numGameThreads; i++) {
    string searchRandSeed = Global::uint64ToString(rand.nextUInt64());
    searches.push_back(new Search(params, nnEval, &logger, searchRandSeed));
  }

  // Check for unused config keys
  cfg.warnUnusedKeys(cerr,&logger);

  if(htmlDir != "")
    MakeDir::make(htmlDir);

  Book* book;
  bool bookFileExists;
  {
    std::ifstream infile;
    bookFileExists = FileUtils::tryOpen(infile,bookFile);
  }
  if(bookFileExists) {
    book = Book::loadFromFile(bookFile,sharpScoreOutlierCap);
    if(
      boardSizeX != book->getInitialHist().getRecentBoard(0).x_size ||
      boardSizeY != book->getInitialHist().getRecentBoard(0).y_size ||
      repBound != book->repBound ||
      rules != book->getInitialHist().rules
    ) {
      throw StringError("Book parameters do not match");
    }
    if(bonusFile != "") {
      if(!bonusInitialBoard.isEqualForTesting(book->getInitialHist().getRecentBoard(0), false, false))
        throw StringError(
          "Book initial board and initial board in bonus sgf file do not match\n" +
          Board::toStringSimple(book->getInitialHist().getRecentBoard(0),'\n') + "\n" +
          Board::toStringSimple(bonusInitialBoard,'\n')
        );
      if(bonusInitialPla != book->initialPla)
        throw StringError(
          "Book initial player and initial player in bonus sgf file do not match\n" +
          PlayerIO::playerToString(book->initialPla) + " book \n" +
          PlayerIO::playerToString(bonusInitialPla) + " bonus"
        );
    }

    if(!allowChangingBookParams) {
      if(
        errorFactor != book->getErrorFactor() ||
        costPerMove != book->getCostPerMove() ||
        costPerUCBWinLossLoss != book->getCostPerUCBWinLossLoss() ||
        costPerUCBWinLossLossPow3 != book->getCostPerUCBWinLossLossPow3() ||
        costPerUCBWinLossLossPow7 != book->getCostPerUCBWinLossLossPow7() ||
        costPerUCBScoreLoss != book->getCostPerUCBScoreLoss() ||
        costPerLogPolicy != book->getCostPerLogPolicy() ||
        costPerMovesExpanded != book->getCostPerMovesExpanded() ||
        costPerSquaredMovesExpanded != book->getCostPerSquaredMovesExpanded() ||
        costWhenPassFavored != book->getCostWhenPassFavored() ||
        bonusPerWinLossError != book->getBonusPerWinLossError() ||
        bonusPerScoreError != book->getBonusPerScoreError() ||
        bonusPerSharpScoreDiscrepancy != book->getBonusPerSharpScoreDiscrepancy() ||
        bonusPerExcessUnexpandedPolicy != book->getBonusPerExcessUnexpandedPolicy() ||
        bonusForWLPV1 != book->getBonusForWLPV1() ||
        bonusForWLPV2 != book->getBonusForWLPV2() ||
        bonusForBiggestWLCost != book->getBonusForBiggestWLCost() ||
        scoreLossCap != book->getScoreLossCap() ||
        utilityPerScore != book->getUtilityPerScore() ||
        policyBoostSoftUtilityScale != book->getPolicyBoostSoftUtilityScale() ||
        utilityPerPolicyForSorting != book->getUtilityPerPolicyForSorting() ||
        maxVisitsForReExpansion != book->getMaxVisitsForReExpansion()
      ) {
        throw StringError("Book parameters do not match");
      }
    }
    else {
      if(errorFactor != book->getErrorFactor()) { logger.write("Changing errorFactor from " + Global::doubleToString(book->getErrorFactor()) + " to " + Global::doubleToString(errorFactor)); book->setErrorFactor(errorFactor); }
      if(costPerMove != book->getCostPerMove()) { logger.write("Changing costPerMove from " + Global::doubleToString(book->getCostPerMove()) + " to " + Global::doubleToString(costPerMove)); book->setCostPerMove(costPerMove); }
      if(costPerUCBWinLossLoss != book->getCostPerUCBWinLossLoss()) { logger.write("Changing costPerUCBWinLossLoss from " + Global::doubleToString(book->getCostPerUCBWinLossLoss()) + " to " + Global::doubleToString(costPerUCBWinLossLoss)); book->setCostPerUCBWinLossLoss(costPerUCBWinLossLoss); }
      if(costPerUCBWinLossLossPow3 != book->getCostPerUCBWinLossLossPow3()) { logger.write("Changing costPerUCBWinLossLossPow3 from " + Global::doubleToString(book->getCostPerUCBWinLossLossPow3()) + " to " + Global::doubleToString(costPerUCBWinLossLossPow3)); book->setCostPerUCBWinLossLossPow3(costPerUCBWinLossLossPow3); }
      if(costPerUCBWinLossLossPow7 != book->getCostPerUCBWinLossLossPow7()) { logger.write("Changing costPerUCBWinLossLossPow7 from " + Global::doubleToString(book->getCostPerUCBWinLossLossPow7()) + " to " + Global::doubleToString(costPerUCBWinLossLossPow7)); book->setCostPerUCBWinLossLossPow7(costPerUCBWinLossLossPow7); }
      if(costPerUCBScoreLoss != book->getCostPerUCBScoreLoss()) { logger.write("Changing costPerUCBScoreLoss from " + Global::doubleToString(book->getCostPerUCBScoreLoss()) + " to " + Global::doubleToString(costPerUCBScoreLoss)); book->setCostPerUCBScoreLoss(costPerUCBScoreLoss); }
      if(costPerLogPolicy != book->getCostPerLogPolicy()) { logger.write("Changing costPerLogPolicy from " + Global::doubleToString(book->getCostPerLogPolicy()) + " to " + Global::doubleToString(costPerLogPolicy)); book->setCostPerLogPolicy(costPerLogPolicy); }
      if(costPerMovesExpanded != book->getCostPerMovesExpanded()) { logger.write("Changing costPerMovesExpanded from " + Global::doubleToString(book->getCostPerMovesExpanded()) + " to " + Global::doubleToString(costPerMovesExpanded)); book->setCostPerMovesExpanded(costPerMovesExpanded); }
      if(costPerSquaredMovesExpanded != book->getCostPerSquaredMovesExpanded()) { logger.write("Changing costPerSquaredMovesExpanded from " + Global::doubleToString(book->getCostPerSquaredMovesExpanded()) + " to " + Global::doubleToString(costPerSquaredMovesExpanded)); book->setCostPerSquaredMovesExpanded(costPerSquaredMovesExpanded); }
      if(costWhenPassFavored != book->getCostWhenPassFavored()) { logger.write("Changing costWhenPassFavored from " + Global::doubleToString(book->getCostWhenPassFavored()) + " to " + Global::doubleToString(costWhenPassFavored)); book->setCostWhenPassFavored(costWhenPassFavored); }
      if(bonusPerWinLossError != book->getBonusPerWinLossError()) { logger.write("Changing bonusPerWinLossError from " + Global::doubleToString(book->getBonusPerWinLossError()) + " to " + Global::doubleToString(bonusPerWinLossError)); book->setBonusPerWinLossError(bonusPerWinLossError); }
      if(bonusPerScoreError != book->getBonusPerScoreError()) { logger.write("Changing bonusPerScoreError from " + Global::doubleToString(book->getBonusPerScoreError()) + " to " + Global::doubleToString(bonusPerScoreError)); book->setBonusPerScoreError(bonusPerScoreError); }
      if(bonusPerSharpScoreDiscrepancy != book->getBonusPerSharpScoreDiscrepancy()) { logger.write("Changing bonusPerSharpScoreDiscrepancy from " + Global::doubleToString(book->getBonusPerSharpScoreDiscrepancy()) + " to " + Global::doubleToString(bonusPerSharpScoreDiscrepancy)); book->setBonusPerSharpScoreDiscrepancy(bonusPerSharpScoreDiscrepancy); }
      if(bonusPerExcessUnexpandedPolicy != book->getBonusPerExcessUnexpandedPolicy()) { logger.write("Changing bonusPerExcessUnexpandedPolicy from " + Global::doubleToString(book->getBonusPerExcessUnexpandedPolicy()) + " to " + Global::doubleToString(bonusPerExcessUnexpandedPolicy)); book->setBonusPerExcessUnexpandedPolicy(bonusPerExcessUnexpandedPolicy); }
      if(bonusForWLPV1 != book->getBonusForWLPV1()) { logger.write("Changing bonusForWLPV1 from " + Global::doubleToString(book->getBonusForWLPV1()) + " to " + Global::doubleToString(bonusForWLPV1)); book->setBonusForWLPV1(bonusForWLPV1); }
      if(bonusForWLPV2 != book->getBonusForWLPV2()) { logger.write("Changing bonusForWLPV2 from " + Global::doubleToString(book->getBonusForWLPV2()) + " to " + Global::doubleToString(bonusForWLPV2)); book->setBonusForWLPV2(bonusForWLPV2); }
      if(bonusForBiggestWLCost != book->getBonusForBiggestWLCost()) { logger.write("Changing bonusForBiggestWLCost from " + Global::doubleToString(book->getBonusForBiggestWLCost()) + " to " + Global::doubleToString(bonusForBiggestWLCost)); book->setBonusForBiggestWLCost(bonusForBiggestWLCost); }
      if(scoreLossCap != book->getScoreLossCap()) { logger.write("Changing scoreLossCap from " + Global::doubleToString(book->getScoreLossCap()) + " to " + Global::doubleToString(scoreLossCap)); book->setScoreLossCap(scoreLossCap); }
      if(utilityPerScore != book->getUtilityPerScore()) { logger.write("Changing utilityPerScore from " + Global::doubleToString(book->getUtilityPerScore()) + " to " + Global::doubleToString(utilityPerScore)); book->setUtilityPerScore(utilityPerScore); }
      if(policyBoostSoftUtilityScale != book->getPolicyBoostSoftUtilityScale()) { logger.write("Changing policyBoostSoftUtilityScale from " + Global::doubleToString(book->getPolicyBoostSoftUtilityScale()) + " to " + Global::doubleToString(policyBoostSoftUtilityScale)); book->setPolicyBoostSoftUtilityScale(policyBoostSoftUtilityScale); }
      if(utilityPerPolicyForSorting != book->getUtilityPerPolicyForSorting()) { logger.write("Changing utilityPerPolicyForSorting from " + Global::doubleToString(book->getUtilityPerPolicyForSorting()) + " to " + Global::doubleToString(utilityPerPolicyForSorting)); book->setUtilityPerPolicyForSorting(utilityPerPolicyForSorting); }
      if(maxVisitsForReExpansion != book->getMaxVisitsForReExpansion()) { logger.write("Changing maxVisitsForReExpansion from " + Global::doubleToString(book->getMaxVisitsForReExpansion()) + " to " + Global::doubleToString(maxVisitsForReExpansion)); book->setMaxVisitsForReExpansion(maxVisitsForReExpansion); }

    }
    logger.write("Loaded preexisting book with " + Global::uint64ToString(book->size()) + " nodes from " + bookFile);
    logger.write("Book version = " + Global::intToString(book->bookVersion));
  }
  else {
    {
      ostringstream bout;
      Board::printBoard(bout, bonusInitialBoard, Board::NULL_LOC, NULL);
      logger.write("Initializing new book with starting position:\n" + bout.str());
    }
    book = new Book(
      Book::LATEST_BOOK_VERSION,
      bonusInitialBoard,
      rules,
      bonusInitialPla,
      repBound,
      errorFactor,
      costPerMove,
      costPerUCBWinLossLoss,
      costPerUCBWinLossLossPow3,
      costPerUCBWinLossLossPow7,
      costPerUCBScoreLoss,
      costPerLogPolicy,
      costPerMovesExpanded,
      costPerSquaredMovesExpanded,
      costWhenPassFavored,
      bonusPerWinLossError,
      bonusPerScoreError,
      bonusPerSharpScoreDiscrepancy,
      bonusPerExcessUnexpandedPolicy,
      bonusForWLPV1,
      bonusForWLPV2,
      bonusForBiggestWLCost,
      scoreLossCap,
      utilityPerScore,
      policyBoostSoftUtilityScale,
      utilityPerPolicyForSorting,
      maxVisitsForReExpansion,
      sharpScoreOutlierCap
    );
    logger.write("Creating new book at " + bookFile);
    book->saveToFile(bookFile);
    ofstream out;
    FileUtils::open(out,bookFile + ".cfg");
    out << cfg.getContents() << endl;
    out.close();
  }

  Book* traceBook = NULL;
  if(traceBookFile.size() > 0) {
    if(numIterations > 0)
      throw StringError("Cannot specify iterations and trace book at the same time");
    traceBook = Book::loadFromFile(traceBookFile,sharpScoreOutlierCap);
    traceBook->recomputeEverything();
    logger.write("Loaded trace book with " + Global::uint64ToString(book->size()) + " nodes from " + traceBookFile);
    logger.write("traceBookMinVisits = " + Global::doubleToString(traceBookMinVisits));
  }

  book->setBonusByHash(bonusByHash);
  book->recomputeEverything();

  if(!std::atomic_is_lock_free(&shouldStop))
    throw StringError("shouldStop is not lock free, signal-quitting mechanism for terminating matches will NOT work!");
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  const PrintTreeOptions options;
  const Player perspective = P_WHITE;

  // ClockTimer timer;
  std::mutex bookMutex;

  // Avoid all moves that are currently in the book on this node,
  // unless allowReExpansion is true and this node qualifies for the visit threshold for allowReExpansion and
  // to re-search already searched moves freshly.
  // Mark avoidMoveUntilByLoc to be passed to search so that we only search new stuff.
  auto findNewMovesAlreadyLocked = [&](
    const BoardHistory& hist,
    ConstSymBookNode constNode,
    bool allowReExpansion,
    std::vector<int>& avoidMoveUntilByLoc,
    bool& isReExpansion
  ) {
    avoidMoveUntilByLoc = std::vector<int>(Board::MAX_ARR_SIZE,0);
    isReExpansion = allowReExpansion && constNode.canReExpand() && constNode.recursiveValues().visits < book->getMaxVisitsForReExpansion();
    Player pla = hist.presumedNextMovePla;
    Board board = hist.getRecentBoard(0);
    bool hasAtLeastOneLegalNewMove = false;
    for(Loc moveLoc = 0; moveLoc < Board::MAX_ARR_SIZE; moveLoc++) {
      if(hist.isLegal(board,moveLoc,pla)) {
        if(!isReExpansion && constNode.isMoveInBook(moveLoc))
          avoidMoveUntilByLoc[moveLoc] = 1;
        else
          hasAtLeastOneLegalNewMove = true;
      }
    }
    return hasAtLeastOneLegalNewMove;
  };

  auto setParamsAndAvoidMovesCompensatingCpuct = [&](Search* search, SearchParams thisParams, const std::vector<int>& avoidMoveUntilByLoc) {
    Board board = search->getRootBoard();
    BoardHistory hist = search->getRootHist();
    Player pla = search->getRootPla();
    bool includeOwnerMap = false;
    std::shared_ptr<NNOutput> result = PlayUtils::getFullSymmetryNNOutput(board, hist, pla, includeOwnerMap, search->nnEvaluator);
    double policySum = 0.0;
    for(Loc loc = 0; loc<Board::MAX_ARR_SIZE; loc++) {
      if(avoidMoveUntilByLoc[loc] <= 0) {
        int pos = search->getPos(loc);
        if(result->policyProbs[pos] > 0) {
          policySum += result->policyProbs[pos];
        }
      }
    }
    policySum = std::max(policySum, 1e-5);
    policySum = std::min(policySum, 1.0);
    policySum = (float)pow(policySum, 1.0 / (4.0*thisParams.wideRootNoise + 1.0));

    thisParams.cpuctExploration /= policySum;
    thisParams.cpuctExplorationLog /= policySum;
    search->setParams(thisParams);
    search->setAvoidMoveUntilByLoc(avoidMoveUntilByLoc, avoidMoveUntilByLoc);
  };

  auto setNodeThisValuesNoMoves = [&](SymBookNode node) {
    std::lock_guard<std::mutex> lock(bookMutex);
    BookValues& nodeValues = node.thisValuesNotInBook();
    if(node.pla() == P_WHITE) {
      nodeValues.winLossValue = -1e20;
      nodeValues.scoreMean = -1e20;
      nodeValues.sharpScoreMean =  -1e20;
    }
    else {
      nodeValues.winLossValue = 1e20;
      nodeValues.scoreMean = 1e20;
      nodeValues.sharpScoreMean =  1e20;
    }
    nodeValues.winLossError = 0.0;
    nodeValues.scoreError = 0.0;
    nodeValues.scoreStdev = 0.0;
    nodeValues.maxPolicy = 0.0;
    nodeValues.weight = 0.0;
    nodeValues.visits = 0.0;

    node.canExpand() = false;
  };

  auto setNodeThisValuesTerminal = [&](SymBookNode node, const BoardHistory& hist) {
    assert(hist.isGameFinished);

    std::lock_guard<std::mutex> lock(bookMutex);
    BookValues& nodeValues = node.thisValuesNotInBook();
    if(hist.isNoResult) {
      nodeValues.winLossValue = 0.0;
      nodeValues.scoreMean = 0.0;
      nodeValues.sharpScoreMean = 0.0;
    }
    else {
      if(hist.winner == P_WHITE) {
        assert(hist.finalWhiteMinusBlackScore > 0.0);
        nodeValues.winLossValue = 1.0;
      }
      else if(hist.winner == P_BLACK) {
        assert(hist.finalWhiteMinusBlackScore < 0.0);
        nodeValues.winLossValue = -1.0;
      }
      else {
        assert(hist.finalWhiteMinusBlackScore == 0.0);
        nodeValues.winLossValue = 0.0;
      }
      nodeValues.scoreMean = hist.finalWhiteMinusBlackScore;
      nodeValues.sharpScoreMean = hist.finalWhiteMinusBlackScore;
    }

    nodeValues.winLossError = 0.0;
    nodeValues.scoreError = 0.0;
    nodeValues.scoreStdev = 0.0;
    nodeValues.maxPolicy = 1.0;
    double visits = maxVisitsForLeaves;
    nodeValues.weight = visits;
    nodeValues.visits = visits;

    node.canExpand() = false;
  };

  auto setNodeThisValuesFromFinishedSearch = [&](
    SymBookNode node,
    Search* search,
    const SearchNode* searchNode,
    const Board& board,
    const BoardHistory& hist,
    const std::vector<int>& avoidMoveUntilByLoc
  ) {
    // Get root values
    ReportedSearchValues remainingSearchValues;
    bool getSuc = search->getPrunedNodeValues(searchNode,remainingSearchValues);
    // Something is bad if this is false, since we should be searching with positive visits
    // or otherwise this searchNode must be a terminal node with visits from a deeper search.
    assert(getSuc);
    (void)getSuc;
    double sharpScore = 0.0;
    // cout << "Calling sharpscore " << timer.getSeconds() << endl;
    getSuc = search->getSharpScore(searchNode,sharpScore);
    // cout << "Done sharpscore " << timer.getSeconds() << endl;
    assert(getSuc);
    (void)getSuc;

    // cout << "Calling shallowAvg " << timer.getSeconds() << endl;
    std::pair<double,double> errors = search->getShallowAverageShorttermWLAndScoreError(searchNode);
    // cout << "Done shallowAvg " << timer.getSeconds() << endl;

    // Use full symmetry for the policy for nodes we record for the book
    bool includeOwnerMap = false;
    // cout << "Calling full nn " << timer.getSeconds() << endl;
    std::shared_ptr<NNOutput> fullSymNNOutput = PlayUtils::getFullSymmetryNNOutput(board, hist, node.pla(), includeOwnerMap, search->nnEvaluator);
    float policyProbs[NNPos::MAX_NN_POLICY_SIZE];
    std::copy(fullSymNNOutput->policyProbs, fullSymNNOutput->policyProbs+NNPos::MAX_NN_POLICY_SIZE, policyProbs);
    // cout << "Done full nn " << timer.getSeconds() << endl;

    // Zero out all the policies for moves we already have, we want the max *remaining* policy
    if(avoidMoveUntilByLoc.size() > 0) {
      assert(avoidMoveUntilByLoc.size() == Board::MAX_ARR_SIZE);
      for(Loc loc = 0; loc<Board::MAX_ARR_SIZE; loc++) {
        if(avoidMoveUntilByLoc[loc] > 0) {
          int pos = search->getPos(loc);
          assert(pos >= 0 && pos < NNPos::MAX_NN_POLICY_SIZE);
          policyProbs[pos] = -1;
        }
      }
    }
    double maxPolicy = getMaxPolicy(policyProbs);
    assert(maxPolicy >= 0.0);

    // LOCK BOOK AND UPDATE -------------------------------------------------------
    std::lock_guard<std::mutex> lock(bookMutex);

    // Record those values to the book
    BookValues& nodeValues = node.thisValuesNotInBook();
    nodeValues.winLossValue = remainingSearchValues.winLossValue;
    nodeValues.scoreMean = remainingSearchValues.expectedScore;
    nodeValues.sharpScoreMean = sharpScore;
    nodeValues.winLossError = errors.first;
    nodeValues.scoreError = errors.second;
    nodeValues.scoreStdev = remainingSearchValues.expectedScoreStdev;

    nodeValues.maxPolicy = maxPolicy;
    nodeValues.weight = remainingSearchValues.weight;
    nodeValues.visits = (double)remainingSearchValues.visits;
  };


  // Perform a short search and update thisValuesNotInBook for a node
  auto searchAndUpdateNodeThisValues = [&](Search* search, SymBookNode node) {
    ConstSymBookNode constNode(node);
    BoardHistory hist;
    std::vector<int> symmetries;
    {
      std::lock_guard<std::mutex> lock(bookMutex);
      std::vector<Loc> moveHistory;
      bool suc = node.getBoardHistoryReachingHere(hist,moveHistory);
      if(!suc) {
        logger.write("WARNING: Failed to get board history reaching node when trying to export to trace book, probably there is some bug");
        logger.write("or else some hash collision or something else is wrong.");
        logger.write("BookHash of node unable to expand: " + node.hash().toString());
        throw StringError("Terminating since there's not a good way to put the book back into a good state with this node unupdated");
      }
      symmetries = constNode.getSymmetries();
    }

    Player pla = hist.presumedNextMovePla;
    Board board = hist.getRecentBoard(0);
    search->setPosition(pla,board,hist);
    search->setRootSymmetryPruningOnly(symmetries);

    // Directly set the values for a terminal position
    if(hist.isGameFinished) {
      setNodeThisValuesTerminal(node,hist);
      return;
    }

    std::vector<int> avoidMoveUntilByLoc;
    bool foundNewMoves;
    {
      const bool allowReExpansion = false;
      bool isReExpansion;
      std::lock_guard<std::mutex> lock(bookMutex);
      foundNewMoves = findNewMovesAlreadyLocked(hist,constNode,allowReExpansion,avoidMoveUntilByLoc,isReExpansion);
    }

    if(!foundNewMoves) {
      setNodeThisValuesNoMoves(node);
    }
    else {
      {
        SearchParams thisParams = params;
        thisParams.maxVisits = std::min(params.maxVisits, maxVisitsForLeaves);
        setParamsAndAvoidMovesCompensatingCpuct(search,thisParams,avoidMoveUntilByLoc);
        // cout << "Search and update" << timer.getSeconds() << endl;
        search->runWholeSearch(search->rootPla);
        // cout << "Search and update done" << timer.getSeconds() << endl;
      }

      if(logSearchInfo) {
        std::lock_guard<std::mutex> lock(bookMutex);
        logger.write("Quick search on remaining moves");
        ostringstream out;
        search->printTree(out, search->rootNode, options, perspective);
        logger.write(out.str());
      }

      // Stick all the new values into the book node
      setNodeThisValuesFromFinishedSearch(node, search, search->getRootNode(), search->getRootBoard(), search->getRootHist(), avoidMoveUntilByLoc);
    }
  };

  auto addVariationToBookWithoutUpdate = [&](int gameThreadIdx, const BoardHistory& targetHist, std::set<BookHash>& nodesHashesToUpdate) {
    std::unique_lock<std::mutex> lock(bookMutex);

    Search* search = searches[gameThreadIdx];
    SymBookNode node = book->getRoot();
    BoardHistory hist = book->getInitialHist();
    Player pla = hist.presumedNextMovePla;
    Board board = hist.getRecentBoard(0);
    search->setPosition(pla,board,hist);

    // Run some basic error checking
    if(
      targetHist.initialBoard.pos_hash != board.pos_hash ||
      targetHist.initialBoard.ko_loc != board.ko_loc ||
      targetHist.initialPla != pla ||
      targetHist.initialEncorePhase != hist.initialEncorePhase
    ) {
      throw StringError("Target board history to add to book doesn't start from the same position");
    }
    assert(hist.moveHistory.size() == 0);

    for(auto& move: targetHist.moveHistory) {
      // Make sure we don't walk off the edge under this ruleset.
      if(hist.isGameFinished || hist.isPastNormalPhaseEnd) {
        logger.write("Skipping trace variation at this book hash " + node.hash().toString() + " since game over");
        node.canExpand() = false;
        break;
      }

      Loc moveLoc = move.loc;
      Player movePla = move.pla;
      if(movePla != pla)
        throw StringError("Target board history to add player got out of sync");
      if(movePla != node.pla())
        throw StringError("Target board history to add player got out of sync with node");

      // Illegal move, possibly due to rules mismatch between the books. In that case, we just stop where we are.
      if(!hist.isLegal(board,moveLoc,movePla)) {
        logger.write("Skipping trace variation at this book hash " + node.hash().toString() + " since illegal");
        break;
      }

      if(!node.isMoveInBook(moveLoc)) {
        // If this node in this book or under this ruleset is nonexpandable, then although we can
        // follow existing moves, we can't add any moves.
        if(!node.canExpand()) {
          logger.write("Skipping trace variation at this book hash " + node.hash().toString() + " since nonexpandable");
          break;
        }

        // UNLOCK for performing expensive symmetry computations
        lock.unlock();

        // To avoid oddities in positions where the rules mismatch, expand every move with a noticeably higher raw policy
        // Average all 8 symmetries
        const bool includeOwnerMap = false;
        std::shared_ptr<NNOutput> result = PlayUtils::getFullSymmetryNNOutput(board, hist, pla, includeOwnerMap, nnEval);
        const float* policyProbs = result->policyProbs;
        float moveLocPolicy = policyProbs[search->getPos(moveLoc)];
        assert(moveLocPolicy >= 0);
        vector<std::pair<Loc,float>> extraMoveLocsToExpand;
        for(int pos = 0; pos<NNPos::MAX_NN_POLICY_SIZE; pos++) {
          Loc loc = NNPos::posToLoc(pos, board.x_size, board.y_size, result->nnXLen, result->nnYLen);
          if(loc == Board::NULL_LOC || loc == moveLoc)
            continue;
          if(policyProbs[pos] > 0.0 && policyProbs[pos] > 1.5 * moveLocPolicy + 0.05f)
            extraMoveLocsToExpand.push_back(std::make_pair(loc,policyProbs[pos]));
        }
        std::sort(
          extraMoveLocsToExpand.begin(),
          extraMoveLocsToExpand.end(),
          [](std::pair<Loc,float>& p0, std::pair<Loc,float>& p1) {
            return p0.second > p1.second;
          }
        );

        // LOCK for going back to modifying the book and other shared state
        lock.lock();

        // We're adding moves to this node, so it needs update
        nodesHashesToUpdate.insert(node.hash());

        {
          // Possibly another thread added it, so we need to check again.
          if(!node.isMoveInBook(moveLoc)) {
            Board boardCopy = board;
            BoardHistory histCopy = hist;
            bool childIsTransposing;
            SymBookNode child = node.playAndAddMove(boardCopy,histCopy,moveLoc,moveLocPolicy,childIsTransposing);
            if(!child.isNull() && !childIsTransposing)
              nodesHashesToUpdate.insert(child.hash());
          }
        }
        for(std::pair<Loc,float>& extraMoveLocToExpand: extraMoveLocsToExpand) {
          // Possibly we added it via symmetry, or maybe even another thread, so we need to check again.
          if(!node.isMoveInBook(extraMoveLocToExpand.first)) {
            Board boardCopy = board;
            BoardHistory histCopy = hist;
            bool childIsTransposing;
            SymBookNode child = node.playAndAddMove(boardCopy,histCopy,extraMoveLocToExpand.first,extraMoveLocToExpand.second,childIsTransposing);
            if(!child.isNull() && !childIsTransposing)
              nodesHashesToUpdate.insert(child.hash());
          }
        }
      }

      assert(node.isMoveInBook(moveLoc));
      node = node.playMove(board,hist,moveLoc);
      assert(!node.isNull());
      pla = getOpp(pla);
    }
  };

  // Returns true if any child was added directly to this node (doesn't count recursive stuff).
  std::function<bool(
    Search*, const SearchNode*, SymBookNode,
    const Board&, const BoardHistory&, int,
    std::set<BookHash>&, std::set<BookHash>&,
    std::set<const SearchNode*>&
  )> expandFromSearchResultRecursively;
  expandFromSearchResultRecursively = [&](
    Search* search, const SearchNode* searchNode, SymBookNode node,
    const Board& board, const BoardHistory& hist, int maxDepth,
    std::set<BookHash>& nodesHashesToSearch, std::set<BookHash>& nodesHashesToUpdate,
    std::set<const SearchNode*>& searchNodesRecursedOn
  ) {
    // cout << "Entering expandFromSearchResultRecursively " << timer.getSeconds() << endl;

    if(maxDepth <= 0)
      return false;
    // Quit out immediately when handling transpositions in graph search
    if(searchNodesRecursedOn.find(searchNode) != searchNodesRecursedOn.end())
      return false;
    searchNodesRecursedOn.insert(searchNode);

    assert(searchNode != NULL);
    assert(searchNode->nextPla == node.pla());

    vector<Loc> locs;
    vector<double> playSelectionValues;
    const double scaleMaxToAtLeast = 0.0;
    const bool allowDirectPolicyMoves = false;
    bool suc = search->getPlaySelectionValues(*searchNode, locs, playSelectionValues, NULL, scaleMaxToAtLeast, allowDirectPolicyMoves);
    assert(suc);
    // Possible if this was a terminal node
    if(!suc)
      return false;

    // Find best move
    double bestValue = playSelectionValues[0];
    int bestIdx = 0;
    for(int i = 1; i<playSelectionValues.size(); i++) {
      if(playSelectionValues[i] > bestValue) {
        bestValue = playSelectionValues[i];
        bestIdx = i;
      }
    }
    Loc bestLoc = locs[bestIdx];

    int childrenCapacity;
    const SearchChildPointer* children = searchNode->getChildren(childrenCapacity);
    int numChildren = SearchNode::iterateAndCountChildrenInArray(children,childrenCapacity);

    const NNOutput* nnOutput = searchNode->getNNOutput();
    if(numChildren <= 0 || nnOutput == nullptr)
      return false;

    // Use full symmetry for the policy for nodes we record for the book
    bool includeOwnerMap = false;
    std::shared_ptr<NNOutput> fullSymNNOutput = PlayUtils::getFullSymmetryNNOutput(board, hist, node.pla(), includeOwnerMap, search->nnEvaluator);
    const float* policyProbs = fullSymNNOutput->policyProbs;

    bool anyRecursion = false;
    bool anythingAdded = false;
    // cout << "expandFromSearchResultRecursively begin loop over children " << timer.getSeconds() << endl;
    for(int i = 0; i<numChildren; i++) {
      const SearchNode* childSearchNode = children[i].getIfAllocated();
      Loc moveLoc = children[i].getMoveLoc();
      double rawPolicy = policyProbs[search->getPos(bestLoc)];
      int64_t childVisits = childSearchNode->stats.visits.load(std::memory_order_acquire);

      // Add any child nodes that have enough visits or are the best move, if present.
      if(moveLoc == bestLoc || childVisits >= minTreeVisitsToRecord) {
        SymBookNode child;
        Board nextBoard = board;
        BoardHistory nextHist = hist;

        {
          std::unique_lock<std::mutex> lock(bookMutex);

          if(node.isMoveInBook(moveLoc)) {
            child = node.follow(moveLoc);
            if(!nextHist.isLegal(nextBoard,moveLoc,node.pla())) {
              logger.write("WARNING: Illegal move " + Location::toString(moveLoc, nextBoard));
              ostringstream debugOut;
              nextHist.printDebugInfo(debugOut,nextBoard);
              logger.write(debugOut.str());
              logger.write("BookHash of parent: " + node.hash().toString());
              logger.write("Marking node as done so we don't try to expand it again, but something is probably wrong.");
              node.canExpand() = false;
            }
            nextHist.makeBoardMoveAssumeLegal(nextBoard,moveLoc,node.pla(),nullptr);
            // Overwrite the child if has no moves yet and we searched it deeper
            if(child.numUniqueMovesInBook() == 0 && child.recursiveValues().visits < childSearchNode->stats.visits.load(std::memory_order_acquire)) {
              // No longer need lock here, setNodeThisValuesFromFinishedSearch will lock on its own.
              lock.unlock();
              // Carefully use an empty vector for the avoidMoveUntilByLoc, since the child didn't avoid any moves.
              std::vector<int> childAvoidMoveUntilByLoc;
              setNodeThisValuesFromFinishedSearch(child, search, childSearchNode, nextBoard, nextHist, childAvoidMoveUntilByLoc);
            }
          }
          else {
            // Lock book to add the best child to the book
            bool childIsTransposing;
            {
              assert(!node.isMoveInBook(moveLoc));
              child = node.playAndAddMove(nextBoard, nextHist, moveLoc, rawPolicy, childIsTransposing);
              // Somehow child was illegal?
              if(child.isNull()) {
                logger.write("WARNING: Illegal move " + Location::toString(moveLoc, nextBoard));
                ostringstream debugOut;
                nextHist.printDebugInfo(debugOut,nextBoard);
                logger.write(debugOut.str());
                logger.write("BookHash of parent: " + node.hash().toString());
                logger.write("Marking node as done so we don't try to expand it again, but something is probably wrong.");
                node.canExpand() = false;
              }
              nodesHashesToUpdate.insert(child.hash());
              logger.write("Adding " + node.hash().toString() + " -> " + child.hash().toString() + " move " + Location::toString(moveLoc,board));
              // cout << "Adding " << timer.getSeconds() << endl;
              anythingAdded = true;
            }

            // Stick all the new values into the child node, UNLESS the child already had its own search (i.e. we're just transposing)
            // Unless the child is a leaf and we have more visits than it.
            if(!childIsTransposing || (child.numUniqueMovesInBook() == 0 && child.recursiveValues().visits < childSearchNode->stats.visits.load(std::memory_order_acquire))) {
              // No longer need lock here, setNodeThisValuesFromFinishedSearch will lock on its own.
              lock.unlock();
              // Carefully use an empty vector for the avoidMoveUntilByLoc, since the child didn't avoid any moves.
              std::vector<int> childAvoidMoveUntilByLoc;
              // cout << "Calling setNodeThisValuesFromFinishedSearch " << timer.getSeconds() << endl;
              setNodeThisValuesFromFinishedSearch(child, search, childSearchNode, nextBoard, nextHist, childAvoidMoveUntilByLoc);
              // cout << "Returned from setNodeThisValuesFromFinishedSearch " << timer.getSeconds() << endl;
            }
          }
        } // Release lock

        // Recursively record children with enough visits
        if(maxDepth > 0 && childVisits >= minTreeVisitsToRecord) {
          anyRecursion = true;
          // cout << "Calling expandFromSearchResultRecursively " << maxDepth << " " << childVisits << " " << timer.getSeconds() << endl;
          expandFromSearchResultRecursively(
            search, childSearchNode, child, nextBoard, nextHist, maxDepth-1,
            nodesHashesToSearch, nodesHashesToUpdate, searchNodesRecursedOn
          );
          // cout << "Returned from expandFromSearchResultRecursively " << maxDepth << " " << childVisits << " " << timer.getSeconds() << endl;
        }
      }
    }

    // This node's values need to be recomputed at the end if it changed or anything under it changed.
    if(anythingAdded || anyRecursion)
      nodesHashesToUpdate.insert(node.hash());

    // This node needs to be searched with its new avoid moves if any move was added to update its thisnodevalues.
    if(anythingAdded)
      nodesHashesToSearch.insert(node.hash());

    return anythingAdded;
  };

  auto expandNode = [&](int gameThreadIdx, SymBookNode node, std::vector<SymBookNode>& newAndChangedNodes) {
    ConstSymBookNode constNode(node);

    BoardHistory hist;
    std::vector<Loc> moveHistory;
    std::vector<int> symmetries;
    bool suc;
    {
      std::lock_guard<std::mutex> lock(bookMutex);
      suc = constNode.getBoardHistoryReachingHere(hist,moveHistory);
      symmetries = constNode.getSymmetries();
    }

    if(!suc) {
      std::lock_guard<std::mutex> lock(bookMutex);
      logger.write("WARNING: Failed to get board history reaching node when trying to expand book, probably there is some bug");
      logger.write("or else some hash collision or something else is wrong.");
      logger.write("BookHash of node unable to expand: " + constNode.hash().toString());
      ostringstream movesOut;
      for(Loc move: moveHistory)
        movesOut << Location::toString(move,book->initialBoard) << " ";
      logger.write("Moves:");
      logger.write(movesOut.str());
      logger.write("Marking node as done so we don't try to expand it again, but something is probably wrong.");
      node.canExpand() = false;
      return;
    }

    // Book integrity check, only for later versions since older versions had a bug that gets them permanently with
    // hashes stuck to be bad.
    if(book->bookVersion >= 2) {
      BookHash hashRet;
      int symmetryToAlignRet;
      vector<int> symmetriesRet;
      BookHash::getHashAndSymmetry(hist, book->repBound, hashRet, symmetryToAlignRet, symmetriesRet, book->bookVersion);
      if(hashRet != node.hash()) {
        ostringstream out;
        Board board = hist.getRecentBoard(0);
        Board::printBoard(out, board, Board::NULL_LOC, NULL);
        for(Loc move: moveHistory)
          out << Location::toString(move,book->initialBoard) << " ";
        logger.write("Moves:");
        logger.write(out.str());
        throw StringError("Book failed integrity check, the node with hash " + node.hash().toString() + " when walked to has hash " + hashRet.toString());
      }
    }

    // Terminal node!
    if(hist.isGameFinished || hist.isPastNormalPhaseEnd) {
      std::lock_guard<std::mutex> lock(bookMutex);
      node.canExpand() = false;
      return;
    }

    Search* search = searches[gameThreadIdx];
    Player pla = hist.presumedNextMovePla;
    Board board = hist.getRecentBoard(0);
    search->setPosition(pla,board,hist);
    search->setRootSymmetryPruningOnly(symmetries);

    {
      ostringstream out;
      Board::printBoard(out, board, Board::NULL_LOC, NULL);
      std::lock_guard<std::mutex> lock(bookMutex);
      logger.write("Expanding " + node.hash().toString() + " cost " + Global::doubleToString(node.totalExpansionCost()));
      logger.write(out.str());
    }

    std::vector<int> avoidMoveUntilByLoc;
    bool foundNewMoves;
    bool isReExpansion;
    {
      const bool allowReExpansion = true;
      std::lock_guard<std::mutex> lock(bookMutex);
      foundNewMoves = findNewMovesAlreadyLocked(hist,constNode,allowReExpansion,avoidMoveUntilByLoc,isReExpansion);
    }
    if(!foundNewMoves) {
      std::lock_guard<std::mutex> lock(bookMutex);
      node.canExpand() = false;
      return;
    }

    SearchParams thisParams = params;
    thisParams.wideRootNoise = wideRootNoiseBookExplore;
    thisParams.cpuctExplorationLog = cpuctExplorationLogBookExplore;
    setParamsAndAvoidMovesCompensatingCpuct(search,thisParams,avoidMoveUntilByLoc);
    search->runWholeSearch(search->rootPla);


    if(shouldStop.load(std::memory_order_acquire))
      return;

    if(logSearchInfo) {
      std::lock_guard<std::mutex> lock(bookMutex);
      ostringstream out;
      search->printTree(out, search->rootNode, options, perspective);
      logger.write("Search result");
      logger.write(out.str());
    }

    // cout << "Beginning recurison " << timer.getSeconds() << endl;

    std::set<BookHash> nodesHashesToSearch;
    std::set<BookHash> nodesHashesToUpdate;
    std::set<const SearchNode*> searchNodesRecursedOn;
    bool anythingAdded = expandFromSearchResultRecursively(
      search, search->rootNode, node, board, hist, maxDepthToRecord,
      nodesHashesToSearch, nodesHashesToUpdate, searchNodesRecursedOn
    );

    // cout << "Ending recursion " << timer.getSeconds() << endl;

    // We should always be newly leaf searching and updating this node since we added something to it.
    assert(nodesHashesToSearch.find(node.hash()) != nodesHashesToSearch.end());
    assert(nodesHashesToUpdate.find(node.hash()) != nodesHashesToUpdate.end());

    // And immediately do a search to update each node we need to.
    // cout << "Doing searches to update " << timer.getSeconds() << endl;
    for(const BookHash& hash: nodesHashesToSearch) {
      SymBookNode nodeToSearch;
      {
        std::lock_guard<std::mutex> lock(bookMutex);
        nodeToSearch = book->getByHash(hash);
      }
      searchAndUpdateNodeThisValues(search,nodeToSearch);
    }
    // cout << "Done searches to update " << timer.getSeconds() << endl;

    {
      std::lock_guard<std::mutex> lock(bookMutex);
      for(const BookHash& hash: nodesHashesToUpdate) {
        SymBookNode nodeToUpdate;
        nodeToUpdate = book->getByHash(hash);
        newAndChangedNodes.push_back(nodeToUpdate);
      }
    }

    // Only nodes that have never been expanded on their own (were added from another node's search) are allowed for reexpansion.
    node.canReExpand() = false;
    newAndChangedNodes.push_back(node);

    // Make sure to process the nodes to search and updates so the book is in a consistent state, before we do any quitting out.
    // On non-reexpansions, we expect to always add at least one new move to the book for this node.
    if(!anythingAdded && !isReExpansion) {
      std::lock_guard<std::mutex> lock(bookMutex);
      logger.write("WARNING: Could not expand since search obtained no new moves, despite earlier checks about legal moves existing not yet in book");
      logger.write("BookHash of node unable to expand: " + constNode.hash().toString());
      ostringstream debugOut;
      hist.printDebugInfo(debugOut,board);
      logger.write(debugOut.str());
      logger.write("Marking node as done so we don't try to expand it again, but something is probably wrong.");
      node.canExpand() = false;
    }

  };

  if(traceBook != NULL) {
    std::set<BookHash> nodesHashesToUpdate;
    {
      ThreadSafeQueue<SymBookNode> positionsToTrace;
      std::vector<SymBookNode> allNodes = traceBook->getAllLeaves(traceBookMinVisits);
      std::atomic<int64_t> variationsAdded(0);
      auto loopAddingVariations = [&](int gameThreadIdx) {
        while(true) {
          if(shouldStop.load(std::memory_order_acquire))
            return;
          SymBookNode node;
          bool suc = positionsToTrace.tryPop(node);
          if(!suc)
            return;
          BoardHistory hist;
          std::vector<Loc> moveHistory;
          suc = node.getBoardHistoryReachingHere(hist, moveHistory);
          assert(suc);
          (void)suc;
          addVariationToBookWithoutUpdate(gameThreadIdx, hist, nodesHashesToUpdate);
          int64_t currentVariationsAdded = variationsAdded.fetch_add(1) + 1;
          if(currentVariationsAdded % 400 == 0) {
            logger.write(
              "Tracing book, currentVariationsAdded " +
              Global::int64ToString(currentVariationsAdded) + "/" + Global::uint64ToString(allNodes.size())
            );
          }
        }
      };

      for(SymBookNode node: allNodes)
        positionsToTrace.forcePush(node);
      vector<std::thread> threads;
      for(int gameThreadIdx = 0; gameThreadIdx<numGameThreads; gameThreadIdx++) {
        threads.push_back(std::thread(loopAddingVariations, gameThreadIdx));
      }
      for(int gameThreadIdx = 0; gameThreadIdx<numGameThreads; gameThreadIdx++) {
        threads[gameThreadIdx].join();
      }
      int64_t currentVariationsAdded = variationsAdded.load();
      logger.write(
        "Tracing book, currentVariationsAdded " +
        Global::int64ToString(currentVariationsAdded) + "/" + Global::uint64ToString(allNodes.size())
      );
    }
    {
      ThreadSafeQueue<BookHash> hashesToUpdate;
      std::atomic<int64_t> hashesUpdated(0);
      auto loopUpdatingHashes = [&](int gameThreadIdx) {
        while(true) {
          if(shouldStop.load(std::memory_order_acquire))
            return;
          BookHash hash;
          bool suc = hashesToUpdate.tryPop(hash);
          if(!suc)
            return;
          SymBookNode node;
          {
            std::lock_guard<std::mutex> lock(bookMutex);
            node = book->getByHash(hash);
            assert(!node.isNull());
          }
          Search* search = searches[gameThreadIdx];
          searchAndUpdateNodeThisValues(search, node);
          int64_t currentHashesUpdated = hashesUpdated.fetch_add(1) + 1;
          if(currentHashesUpdated % 100 == 0) {
            logger.write(
              "Updating book, currentHashesUpdated " +
              Global::int64ToString(currentHashesUpdated) + "/" + Global::uint64ToString(nodesHashesToUpdate.size())
            );
          }
        }
      };

      for(BookHash hash: nodesHashesToUpdate)
        hashesToUpdate.forcePush(hash);
      vector<std::thread> threads;
      for(int gameThreadIdx = 0; gameThreadIdx<numGameThreads; gameThreadIdx++) {
        threads.push_back(std::thread(loopUpdatingHashes, gameThreadIdx));
      }
      for(int gameThreadIdx = 0; gameThreadIdx<numGameThreads; gameThreadIdx++) {
        threads[gameThreadIdx].join();
      }
      int64_t currentHashesUpdated = hashesUpdated.load();
      logger.write(
        "Tracing book, currentHashesUpdated " +
        Global::int64ToString(currentHashesUpdated) + "/" + Global::uint64ToString(nodesHashesToUpdate.size())
      );
    }

    if(shouldStop.load(std::memory_order_acquire)) {
      logger.write("Trace book incomplete, exiting without saving");
      throw StringError("Trace book incomplete, exiting without saving");
    }

    logger.write("Recomputing recursive values for entire book");
    book->recomputeEverything();
  }
  else {
    ThreadSafeQueue<SymBookNode> positionsToSearch;

    for(int iteration = 0; iteration < numIterations; iteration++) {
      if(shouldStop.load(std::memory_order_acquire))
        break;

      if(iteration % saveEveryIterations == 0 && iteration != 0) {
        logger.write("SAVING TO FILE " + bookFile);
        book->saveToFile(bookFile);
        ofstream out;
        FileUtils::open(out, bookFile + ".cfg");
        out << cfg.getContents() << endl;
        out.close();
      }

      logger.write("BEGINNING BOOK EXPANSION ITERATION " + Global::intToString(iteration));

      std::vector<SymBookNode> nodesToExpand = book->getNextNToExpand(std::min(1+iteration/2,numToExpandPerIteration));
      for(SymBookNode node: nodesToExpand) {
        bool suc = positionsToSearch.forcePush(node);
        assert(suc);
        (void)suc;
      }

      std::vector<SymBookNode> newAndChangedNodes = nodesToExpand;

      auto loopExpandingNodes = [&](int gameThreadIdx) {
        while(true) {
          if(shouldStop.load(std::memory_order_acquire))
            return;
          SymBookNode node;
          bool suc = positionsToSearch.tryPop(node);
          if(!suc)
            return;
          expandNode(gameThreadIdx, node, newAndChangedNodes);
        }
      };

      vector<std::thread> threads;
      for(int gameThreadIdx = 0; gameThreadIdx<numGameThreads; gameThreadIdx++) {
        threads.push_back(std::thread(loopExpandingNodes, gameThreadIdx));
      }
      for(int gameThreadIdx = 0; gameThreadIdx<numGameThreads; gameThreadIdx++) {
        threads[gameThreadIdx].join();
      }

      book->recompute(newAndChangedNodes);
      if(shouldStop.load(std::memory_order_acquire))
        break;
    }
  }

  if(traceBook != NULL || numIterations > 0) {
    logger.write("SAVING TO FILE " + bookFile);
    book->saveToFile(bookFile);
    ofstream out;
    FileUtils::open(out, bookFile + ".cfg");
    out << cfg.getContents() << endl;
    out.close();
  }

  if(htmlDir != "") {
    logger.write("EXPORTING HTML TO " + htmlDir);
    book->exportToHtmlDir(htmlDir,rulesLabel,rulesLink,htmlDevMode,htmlMinVisits,logger);
  }

  for(int i = 0; i<numGameThreads; i++)
    delete searches[i];
  delete nnEval;
  delete book;
  delete traceBook;
  ScoreValue::freeTables();
  logger.write("DONE");
  return 0;
}

int MainCmds::checkbook(const vector<string>& args) {
  Board::initHash();
  ScoreValue::initTables();

  string bookFile;
  try {
    KataGoCommandLine cmd("Check integrity of opening book");

    TCLAP::ValueArg<string> bookFileArg("","book-file","Book file to write to or continue expanding",true,string(),"FILE");
    cmd.add(bookFileArg);

    cmd.parseArgs(args);

    bookFile = bookFileArg.getValue();
  }
  catch (TCLAP::ArgException &e) {
    cerr << "Error: " << e.error() << " for argument " << e.argId() << endl;
    return 1;
  }

  Rand rand;
  const bool logToStdout = true;
  const bool logToStderr = false;
  const bool logTime = false;
  Logger logger(nullptr, logToStdout, logToStderr, logTime);

  Book* book;
  {
    double sharpScoreOutlierCap = 2.0;
    book = Book::loadFromFile(bookFile,sharpScoreOutlierCap);
    logger.write("Loaded preexisting book with " + Global::uint64ToString(book->size()) + " nodes from " + bookFile);
    logger.write("Book version = " + Global::intToString(book->bookVersion));
  }

  const PrintTreeOptions options;

  auto testNode = [&](SymBookNode node) {
    ConstSymBookNode constNode(node);

    BoardHistory hist;
    std::vector<Loc> moveHistory;
    std::vector<int> symmetries;
    bool suc;
    {
      suc = constNode.getBoardHistoryReachingHere(hist,moveHistory);
      symmetries = constNode.getSymmetries();
    }

    if(!suc) {
      logger.write("WARNING: Failed to get board history reaching node, probably there is some bug");
      logger.write("or else some hash collision or something else is wrong.");
      logger.write("BookHash of node unable to expand: " + constNode.hash().toString());
      ostringstream out;
      Board board = hist.getRecentBoard(0);
      Board::printBoard(out, board, Board::NULL_LOC, NULL);
      for(Loc move: moveHistory)
        out << Location::toString(move,book->initialBoard) << " ";
      logger.write("Moves:");
      logger.write(out.str());
    }

    // Book integrity check
    {
      BookHash hashRet;
      int symmetryToAlignRet;
      vector<int> symmetriesRet;
      BookHash::getHashAndSymmetry(hist, book->repBound, hashRet, symmetryToAlignRet, symmetriesRet, book->bookVersion);
      if(hashRet != node.hash()) {
        logger.write("Book failed integrity check, the node with hash " + node.hash().toString() + " when walked to has hash " + hashRet.toString());
        ostringstream out;
        Board board = hist.getRecentBoard(0);
        Board::printBoard(out, board, Board::NULL_LOC, NULL);
        for(Loc move: moveHistory)
          out << Location::toString(move,book->initialBoard) << " ";
        logger.write("Moves:");
        logger.write(out.str());
      }
    }
  };

  std::vector<SymBookNode> allNodes = book->getAllNodes();
  logger.write("Checking book...");
  int64_t numNodesChecked = 0;
  for(SymBookNode node: allNodes) {
    testNode(node);
    numNodesChecked += 1;
    if(numNodesChecked % 10000 == 0)
      logger.write("Checked " + Global::int64ToString(numNodesChecked) + "/" + Global::int64ToString((int64_t)allNodes.size()) + " nodes");
  }

  delete book;
  ScoreValue::freeTables();
  logger.write("DONE");
  return 0;
}

