/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                       */
/*    This file is part of the HiGHS linear optimization suite           */
/*                                                                       */
/*    Written and engineered 2008-2023 by Julian Hall, Ivet Galabova,    */
/*    Leona Gottwald and Michael Feldmeier                               */
/*                                                                       */
/*    Available as open-source under the MIT License                     */
/*                                                                       */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#include "mip/HighsMipSolverData.h"

#include <random>

// #include "lp_data/HighsLpUtils.h"
#include "lp_data/HighsModelUtils.h"
#include "mip/HighsPseudocost.h"
#include "mip/HighsRedcostFixing.h"
#include "parallel/HighsParallel.h"
#include "pdqsort/pdqsort.h"
#include "presolve/HPresolve.h"
#include "util/HighsIntegers.h"

bool HighsMipSolverData::solutionColFeasible(
    const std::vector<double>& solution, double& obj) const {
  if (int(solution.size()) != mipsolver.model_->num_col_) return false;

  HighsCDouble cdouble_obj = 0;
  for (HighsInt i = 0; i != mipsolver.model_->num_col_; ++i) {
    if (solution[i] < mipsolver.model_->col_lower_[i] - feastol) return false;
    if (solution[i] > mipsolver.model_->col_upper_[i] + feastol) return false;
    if (mipsolver.variableType(i) == HighsVarType::kInteger &&
        std::abs(solution[i] - std::floor(solution[i] + 0.5)) > feastol)
      return false;
    cdouble_obj += mipsolver.colCost(i) * solution[i];
  }
  obj = double(cdouble_obj);
  return true;
}

bool HighsMipSolverData::solutionRowFeasible(
    const std::vector<double>& solution) const {
  for (HighsInt i = 0; i != mipsolver.model_->num_row_; ++i) {
    double rowactivity = 0.0;

    HighsInt start = ARstart_[i];
    HighsInt end = ARstart_[i + 1];

    for (HighsInt j = start; j != end; ++j)
      rowactivity += solution[ARindex_[j]] * ARvalue_[j];

    if (rowactivity > mipsolver.rowUpper(i) + feastol) return false;
    if (rowactivity < mipsolver.rowLower(i) - feastol) return false;
  }
  return true;
}

bool HighsMipSolverData::checkSolution(
    const std::vector<double>& solution) const {
  double obj;
  if (!solutionColFeasible(solution, obj)) return false;
  return solutionRowFeasible(solution);
}

bool HighsMipSolverData::trySolution(const std::vector<double>& solution,
                                     const char solution_source) {
  double obj;
  if (!solutionColFeasible(solution, obj)) return false;
  if (!solutionRowFeasible(solution)) return false;
  return addIncumbent(solution, obj, solution_source);
}

void HighsMipSolverData::startAnalyticCenterComputation(
    const highs::parallel::TaskGroup& taskGroup) {
  taskGroup.spawn([&]() {
    // first check if the analytic center computation should be cancelled, e.g.
    // due to early return in the root node evaluation
    Highs ipm;
    ipm.setOptionValue("solver", "ipm");
    ipm.setOptionValue("run_crossover", kHighsOffString);
    ipm.setOptionValue("presolve", "off");
    ipm.setOptionValue("output_flag", false);
    ipm.setOptionValue("ipm_iteration_limit", 200);
    HighsLp lpmodel(*mipsolver.model_);
    lpmodel.col_cost_.assign(lpmodel.num_col_, 0.0);
    ipm.passModel(std::move(lpmodel));

    ipm.run();
    const std::vector<double>& sol = ipm.getSolution().col_value;
    if (HighsInt(sol.size()) != mipsolver.numCol()) return;
    analyticCenterStatus = ipm.getModelStatus();
    analyticCenter = sol;
  });
}

void HighsMipSolverData::finishAnalyticCenterComputation(
    const highs::parallel::TaskGroup& taskGroup) {
  taskGroup.sync();
  analyticCenterComputed = true;
  if (analyticCenterStatus == HighsModelStatus::kOptimal) {
    HighsInt nfixed = 0;
    HighsInt nintfixed = 0;
    for (HighsInt i = 0; i != mipsolver.numCol(); ++i) {
      double boundRange = mipsolver.mipdata_->domain.col_upper_[i] -
                          mipsolver.mipdata_->domain.col_lower_[i];
      if (boundRange == 0.0) continue;

      double tolerance =
          mipsolver.mipdata_->feastol * std::min(boundRange, 1.0);

      if (analyticCenter[i] <= mipsolver.model_->col_lower_[i] + tolerance) {
        mipsolver.mipdata_->domain.changeBound(
            HighsBoundType::kUpper, i, mipsolver.model_->col_lower_[i],
            HighsDomain::Reason::unspecified());
        if (mipsolver.mipdata_->domain.infeasible()) return;
        ++nfixed;
        if (mipsolver.variableType(i) == HighsVarType::kInteger) ++nintfixed;
      } else if (analyticCenter[i] >=
                 mipsolver.model_->col_upper_[i] - tolerance) {
        mipsolver.mipdata_->domain.changeBound(
            HighsBoundType::kLower, i, mipsolver.model_->col_upper_[i],
            HighsDomain::Reason::unspecified());
        if (mipsolver.mipdata_->domain.infeasible()) return;
        ++nfixed;
        if (mipsolver.variableType(i) == HighsVarType::kInteger) ++nintfixed;
      }
    }
    if (nfixed > 0)
      highsLogDev(mipsolver.options_mip_->log_options, HighsLogType::kInfo,
                  "Fixing %" HIGHSINT_FORMAT " columns (%" HIGHSINT_FORMAT
                  " integers) sitting at bound at "
                  "analytic center\n",
                  nfixed, nintfixed);
    mipsolver.mipdata_->domain.propagate();
    if (mipsolver.mipdata_->domain.infeasible()) return;
  }
}

void HighsMipSolverData::startSymmetryDetection(
    const highs::parallel::TaskGroup& taskGroup,
    std::unique_ptr<SymmetryDetectionData>& symData) {
  symData = std::unique_ptr<SymmetryDetectionData>(new SymmetryDetectionData());
  symData->symDetection.loadModelAsGraph(
      mipsolver.mipdata_->presolvedModel,
      mipsolver.options_mip_->small_matrix_value);
  detectSymmetries = symData->symDetection.initializeDetection();

  if (detectSymmetries) {
    taskGroup.spawn([&]() {
      double startTime = mipsolver.timer_.getWallTime();
      // highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kInfo,
      //              "(%4.1fs) Starting symmetry detection\n",
      //              mipsolver.timer_.read(mipsolver.timer_.solve_clock));
      symData->symDetection.run(symData->symmetries);
      symData->detectionTime = mipsolver.timer_.getWallTime() - startTime;
    });
  } else
    symData.reset();
}

void HighsMipSolverData::finishSymmetryDetection(
    const highs::parallel::TaskGroup& taskGroup,
    std::unique_ptr<SymmetryDetectionData>& symData) {
  taskGroup.sync();

  symmetries = std::move(symData->symmetries);
  highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kInfo,
               "\nSymmetry detection completed in %.1fs\n",
               symData->detectionTime);

  if (symmetries.numGenerators == 0) {
    detectSymmetries = false;
    highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kInfo,
                 "No symmetry present\n\n");
  } else if (symmetries.orbitopes.size() == 0) {
    highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kInfo,
                 "Found %" HIGHSINT_FORMAT " generators\n\n",
                 symmetries.numGenerators);

  } else {
    if (symmetries.numPerms != 0) {
      highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kInfo,
                   "Found %" HIGHSINT_FORMAT " generators and %" HIGHSINT_FORMAT
                   " full orbitope(s) acting on %" HIGHSINT_FORMAT
                   " columns\n\n",
                   symmetries.numPerms, (HighsInt)symmetries.orbitopes.size(),
                   (HighsInt)symmetries.columnToOrbitope.size());
    } else {
      highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kInfo,
                   "Found %" HIGHSINT_FORMAT
                   " full orbitope(s) acting on %" HIGHSINT_FORMAT
                   " columns\n\n",
                   (HighsInt)symmetries.orbitopes.size(),
                   (HighsInt)symmetries.columnToOrbitope.size());
    }
  }
  symData.reset();

  for (HighsOrbitopeMatrix& orbitope : symmetries.orbitopes)
    orbitope.determineOrbitopeType(cliquetable);

  if (symmetries.numPerms != 0)
    globalOrbits = symmetries.computeStabilizerOrbits(domain);
}

double HighsMipSolverData::computeNewUpperLimit(double ub, double mip_abs_gap,
                                                double mip_rel_gap) const {
  double new_upper_limit;
  if (objectiveFunction.isIntegral()) {
    new_upper_limit =
        (std::floor(objectiveFunction.integralScale() * ub - 0.5) /
         objectiveFunction.integralScale());

    if (mip_rel_gap != 0.0)
      new_upper_limit = std::min(
          new_upper_limit,
          ub - std::ceil(mip_rel_gap * fabs(ub + mipsolver.model_->offset_) *
                             objectiveFunction.integralScale() -
                         mipsolver.mipdata_->epsilon) /
                   objectiveFunction.integralScale());

    if (mip_abs_gap != 0.0)
      new_upper_limit = std::min(
          new_upper_limit,
          ub - std::ceil(mip_abs_gap * objectiveFunction.integralScale() -
                         mipsolver.mipdata_->epsilon) /
                   objectiveFunction.integralScale());

    // add feasibility tolerance so that the next best integer feasible solution
    // is definitely included in the remaining search
    new_upper_limit += feastol;
  } else {
    new_upper_limit = std::min(ub - feastol, std::nextafter(ub, -kHighsInf));

    if (mip_rel_gap != 0.0)
      new_upper_limit =
          std::min(new_upper_limit,
                   ub - mip_rel_gap * fabs(ub + mipsolver.model_->offset_));

    if (mip_abs_gap != 0.0)
      new_upper_limit = std::min(new_upper_limit, ub - mip_abs_gap);
  }

  return new_upper_limit;
}

bool HighsMipSolverData::moreHeuristicsAllowed() const {
  // in the beginning of the search and in sub-MIP heuristics we only allow
  // what is proportionally for the currently spent effort plus an initial
  // offset. This is because in a sub-MIP we usually do a truncated search and
  // therefore should not extrapolate the time we spent for heuristics as in
  // the other case. Moreover, since we estimate the total effort for
  // exploring the tree based on the weight of the already pruned nodes, the
  // estimated effort the is not expected to be a good prediction in the
  // beginning.
  if (mipsolver.submip) {
    return heuristic_lp_iterations < total_lp_iterations * heuristic_effort;
  } else if (pruned_treeweight < 1e-3 &&
             num_leaves - num_leaves_before_run < 10 &&
             num_nodes - num_nodes_before_run < 1000) {
    // in the main MIP solver allow an initial offset of 10000 heuristic LP
    // iterations
    if (heuristic_lp_iterations <
        total_lp_iterations * heuristic_effort + 10000)
      return true;
  } else if (heuristic_lp_iterations <
             100000 + ((total_lp_iterations - heuristic_lp_iterations -
                        sb_lp_iterations) >>
                       1)) {
    // compute the node LP iterations in the current run as only those should be
    // used when estimating the total required LP iterations to complete the
    // search
    int64_t heur_iters_curr_run =
        heuristic_lp_iterations - heuristic_lp_iterations_before_run;
    int64_t sb_iters_curr_run = sb_lp_iterations - sb_lp_iterations_before_run;
    int64_t node_iters_curr_run = total_lp_iterations -
                                  total_lp_iterations_before_run -
                                  heur_iters_curr_run - sb_iters_curr_run;
    // now estimate the total fraction of LP iterations that we have spent on
    // heuristics by assuming the node iterations of the current run will
    // grow proportional to the pruned weight of the current tree and the
    // iterations spent for anything else are just added as an offset
    double total_heuristic_effort_estim =
        heuristic_lp_iterations /
        ((total_lp_iterations - node_iters_curr_run) +
         node_iters_curr_run / std::max(0.01, double(pruned_treeweight)));
    // since heuristics help most in the beginning of the search, we want to
    // spent the time we have for heuristics in the first 80% of the tree
    // exploration. Additionally we want to spent the proportional effort
    // of heuristics that is allowed in the first 30% of tree exploration as
    // fast as possible, which is why we have the max(0.3/0.8,...).
    // Hence, in the first 30% of the tree exploration we allow to spent all
    // effort available for heuristics in that part of the search as early as
    // possible, whereas after that we allow the part that is proportionally
    // adequate when we want to spent all available time in the first 80%.
    if (total_heuristic_effort_estim <
        std::max(0.3 / 0.8, std::min(double(pruned_treeweight), 0.8) / 0.8) *
            heuristic_effort) {
      // printf(
      //     "heuristic lp iterations: %ld, total_lp_iterations: %ld, "
      //     "total_heur_effort_estim = %.3f%%\n",
      //     heuristic_lp_iterations, total_lp_iterations,
      //     total_heuristic_effort_estim);
      return true;
    }
  }

  return false;
}

void HighsMipSolverData::removeFixedIndices() {
  integral_cols.erase(
      std::remove_if(integral_cols.begin(), integral_cols.end(),
                     [&](HighsInt col) { return domain.isFixed(col); }),
      integral_cols.end());
  integer_cols.erase(
      std::remove_if(integer_cols.begin(), integer_cols.end(),
                     [&](HighsInt col) { return domain.isFixed(col); }),
      integer_cols.end());
  implint_cols.erase(
      std::remove_if(implint_cols.begin(), implint_cols.end(),
                     [&](HighsInt col) { return domain.isFixed(col); }),
      implint_cols.end());
  continuous_cols.erase(
      std::remove_if(continuous_cols.begin(), continuous_cols.end(),
                     [&](HighsInt col) { return domain.isFixed(col); }),
      continuous_cols.end());
}

void HighsMipSolverData::init() {
  postSolveStack.initializeIndexMaps(mipsolver.model_->num_row_,
                                     mipsolver.model_->num_col_);
  mipsolver.orig_model_ = mipsolver.model_;
  feastol = mipsolver.options_mip_->mip_feasibility_tolerance;
  epsilon = mipsolver.options_mip_->small_matrix_value;
  if (mipsolver.clqtableinit)
    cliquetable.buildFrom(mipsolver.orig_model_, *mipsolver.clqtableinit);
  cliquetable.setMinEntriesForParallelism(
      highs::parallel::num_threads() > 1
          ? mipsolver.options_mip_->mip_min_cliquetable_entries_for_parallelism
          : kHighsIInf);
  if (mipsolver.implicinit) implications.buildFrom(*mipsolver.implicinit);
  heuristic_effort = mipsolver.options_mip_->mip_heuristic_effort;
  detectSymmetries = mipsolver.options_mip_->mip_detect_symmetry;

  firstlpsolobj = -kHighsInf;
  rootlpsolobj = -kHighsInf;
  analyticCenterComputed = false;
  analyticCenterStatus = HighsModelStatus::kNotset;
  maxTreeSizeLog2 = 0;
  numRestarts = 0;
  numRestartsRoot = 0;
  numImprovingSols = 0;
  pruned_treeweight = 0;
  avgrootlpiters = 0;
  num_nodes = 0;
  num_nodes_before_run = 0;
  num_leaves = 0;
  num_leaves_before_run = 0;
  total_lp_iterations = 0;
  heuristic_lp_iterations = 0;
  sepa_lp_iterations = 0;
  sb_lp_iterations = 0;
  total_lp_iterations_before_run = 0;
  heuristic_lp_iterations_before_run = 0;
  sepa_lp_iterations_before_run = 0;
  sb_lp_iterations_before_run = 0;
  num_disp_lines = 0;
  numCliqueEntriesAfterPresolve = 0;
  numCliqueEntriesAfterFirstPresolve = 0;
  cliquesExtracted = false;
  rowMatrixSet = false;
  lower_bound = -kHighsInf;
  upper_bound = kHighsInf;
  upper_limit = mipsolver.options_mip_->objective_bound;
  optimality_limit = mipsolver.options_mip_->objective_bound;

  if (mipsolver.options_mip_->mip_report_level == 0)
    dispfreq = 0;
  else if (mipsolver.options_mip_->mip_report_level == 1)
    dispfreq = 2000;
  else
    dispfreq = 100;
}

void HighsMipSolverData::runPresolve() {
#ifdef HIGHS_DEBUGSOL
  bool debugSolActive = false;
  std::swap(debugSolution.debugSolActive, debugSolActive);
#endif

  mipsolver.timer_.start(mipsolver.timer_.presolve_clock);
  presolve::HPresolve presolve;
  presolve.setInput(mipsolver);
  mipsolver.modelstatus_ = presolve.run(postSolveStack);
  presolve_status = presolve.getPresolveStatus();
  mipsolver.timer_.stop(mipsolver.timer_.presolve_clock);

#ifdef HIGHS_DEBUGSOL
  debugSolution.debugSolActive = debugSolActive;
  if (debugSolution.debugSolActive) debugSolution.registerDomain(domain);
  assert(!debugSolution.debugSolActive ||
         checkSolution(debugSolution.debugSolution));
#endif
}

void HighsMipSolverData::runSetup() {
  const HighsLp& model = *mipsolver.model_;

  last_disptime = -kHighsInf;

  // transform the objective limit to the current model
  upper_limit -= mipsolver.model_->offset_;
  optimality_limit -= mipsolver.model_->offset_;
  lower_bound -= mipsolver.model_->offset_;
  upper_bound -= mipsolver.model_->offset_;

  if (numRestarts == 0 &&
      mipsolver.options_mip_->mip_trivial_heuristics != kHighsOffString) {
    // Set up the data to control the trivial heuristics, and record
    // their success/failure. MIP trivial heuristics data exists
    // separately for the original MIP - since that's the whole problem
    // being solved - and any sub-MIPs
    initialiseTrivialHeuristicsStatistics(mip_trivial_heuristics_statistics_);
    initialiseTrivialHeuristicsStatistics(
        submip_trivial_heuristics_statistics_);
  }

  if (mipsolver.solution_objective_ != kHighsInf) {
    incumbent = postSolveStack.getReducedPrimalSolution(mipsolver.solution_);
    // return the objective value in the transformed space
    double solobj =
        mipsolver.solution_objective_ * (int)mipsolver.orig_model_->sense_ -
        mipsolver.model_->offset_;
    bool feasible = mipsolver.bound_violation_ <=
                        mipsolver.options_mip_->mip_feasibility_tolerance &&
                    mipsolver.integrality_violation_ <=
                        mipsolver.options_mip_->mip_feasibility_tolerance &&
                    mipsolver.row_violation_ <=
                        mipsolver.options_mip_->mip_feasibility_tolerance;
    if (numRestarts == 0) {
      highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kInfo,
                   "\nMIP start solution is %s, objective value is %.12g\n",
                   feasible ? "feasible" : "infeasible",
                   mipsolver.solution_objective_);
    }
    if (feasible && solobj < upper_bound) {
      upper_bound = solobj;
      double new_upper_limit = computeNewUpperLimit(solobj, 0.0, 0.0);
      saveReportMipSolution(new_upper_limit);
      if (new_upper_limit < upper_limit) {
        upper_limit = new_upper_limit;
        optimality_limit =
            computeNewUpperLimit(solobj, mipsolver.options_mip_->mip_abs_gap,
                                 mipsolver.options_mip_->mip_rel_gap);
        nodequeue.setOptimalityLimit(optimality_limit);
      }
    }
    if (!mipsolver.submip && feasible && mipsolver.callback_->user_callback &&
        mipsolver.callback_->active[kCallbackMipSolution]) {
      assert(!mipsolver.submip);
      mipsolver.callback_->clearHighsCallbackDataOut();
      mipsolver.callback_->data_out.mip_solution = mipsolver.solution_.data();
      const bool interrupt = interruptFromCallbackWithData(
          kCallbackMipSolution, mipsolver.solution_objective_,
          "Feasible solution");
      assert(!interrupt);
    }
  }

  if (mipsolver.numCol() == 0)
    addIncumbent(std::vector<double>(), 0, kSolutionSourceEmptyMip);

  redcostfixing = HighsRedcostFixing();
  pseudocost = HighsPseudocost(mipsolver);
  nodequeue.setNumCol(mipsolver.numCol());
  nodequeue.setOptimalityLimit(optimality_limit);

  continuous_cols.clear();
  integer_cols.clear();
  implint_cols.clear();
  integral_cols.clear();

  rowMatrixSet = false;
  if (!rowMatrixSet) {
    rowMatrixSet = true;
    highsSparseTranspose(model.num_row_, model.num_col_, model.a_matrix_.start_,
                         model.a_matrix_.index_, model.a_matrix_.value_,
                         ARstart_, ARindex_, ARvalue_);
    uplocks.resize(model.num_col_);
    downlocks.resize(model.num_col_);
    for (HighsInt i = 0; i != model.num_col_; ++i) {
      HighsInt start = model.a_matrix_.start_[i];
      HighsInt end = model.a_matrix_.start_[i + 1];
      for (HighsInt j = start; j != end; ++j) {
        HighsInt row = model.a_matrix_.index_[j];

        if (model.row_lower_[row] != -kHighsInf) {
          if (model.a_matrix_.value_[j] < 0)
            ++uplocks[i];
          else
            ++downlocks[i];
        }
        if (model.row_upper_[row] != kHighsInf) {
          if (model.a_matrix_.value_[j] < 0)
            ++downlocks[i];
          else
            ++uplocks[i];
        }
      }
    }
  }

  rowintegral.resize(mipsolver.model_->num_row_);

  // compute the maximal absolute coefficients to filter propagation
  maxAbsRowCoef.resize(mipsolver.model_->num_row_);
  for (HighsInt i = 0; i != mipsolver.model_->num_row_; ++i) {
    double maxabsval = 0.0;

    HighsInt start = ARstart_[i];
    HighsInt end = ARstart_[i + 1];
    bool integral = true;
    for (HighsInt j = start; j != end; ++j) {
      if (integral) {
        if (mipsolver.variableType(ARindex_[j]) == HighsVarType::kContinuous)
          integral = false;
        else {
          double intval = std::floor(ARvalue_[j] + 0.5);
          if (std::abs(ARvalue_[j] - intval) > epsilon) integral = false;
        }
      }

      maxabsval = std::max(maxabsval, std::abs(ARvalue_[j]));
    }

    if (integral) {
      if (presolvedModel.row_lower_[i] != -kHighsInf)
        presolvedModel.row_lower_[i] =
            std::ceil(presolvedModel.row_lower_[i] - feastol);

      if (presolvedModel.row_upper_[i] != kHighsInf)
        presolvedModel.row_upper_[i] =
            std::floor(presolvedModel.row_upper_[i] + feastol);
    }

    rowintegral[i] = integral;
    maxAbsRowCoef[i] = maxabsval;
  }

  // compute row activities and propagate all rows once
  objectiveFunction.setupCliquePartition(domain, cliquetable);
  domain.setupObjectivePropagation();
  domain.computeRowActivities();
  domain.propagate();
  if (domain.infeasible()) {
    mipsolver.modelstatus_ = HighsModelStatus::kInfeasible;
    lower_bound = kHighsInf;
    pruned_treeweight = 1.0;
    return;
  }

  if (model.num_col_ == 0) {
    mipsolver.modelstatus_ = HighsModelStatus::kOptimal;
    return;
  }

  if (checkLimits()) return;
  // extract cliques if they have not been extracted before

  for (HighsInt col : domain.getChangedCols())
    implications.cleanupVarbounds(col);
  domain.clearChangedCols();

  lp.getLpSolver().setOptionValue("presolve", "off");
  // lp.getLpSolver().setOptionValue("dual_simplex_cost_perturbation_multiplier",
  // 0.0); lp.getLpSolver().setOptionValue("parallel", "on");
  lp.getLpSolver().setOptionValue("simplex_initial_condition_check", false);

  checkObjIntegrality();
  rootlpsol.clear();
  firstlpsol.clear();
  HighsInt numBin = 0;

  maxTreeSizeLog2 = 0;
  for (HighsInt i = 0; i != mipsolver.numCol(); ++i) {
    switch (mipsolver.variableType(i)) {
      case HighsVarType::kContinuous:
        continuous_cols.push_back(i);
        break;
      case HighsVarType::kImplicitInteger:
        implint_cols.push_back(i);
        integral_cols.push_back(i);
        break;
      case HighsVarType::kInteger:
        integer_cols.push_back(i);
        integral_cols.push_back(i);
        maxTreeSizeLog2 += (HighsInt)std::ceil(
            std::log2(std::min(1024.0, 1.0 + mipsolver.model_->col_upper_[i] -
                                           mipsolver.model_->col_lower_[i])));
        // NB Since this is for counting the number of times the
        // condition is true using the bitwise operator avoids having
        // any conditional branch whereas using the logical operator
        // would require a branch due to short circuit
        // evaluation. Semantically both is equivalent and correct. If
        // there was any code to be executed for the condition being
        // true then there would be a conditional branch in any case
        // and I would have used the logical to begin with.
        //
        // Hence any compiler warning can be ignored safely
        numBin +=
            (static_cast<HighsInt>(mipsolver.model_->col_lower_[i] == 0.0) &
             static_cast<HighsInt>(mipsolver.model_->col_upper_[i] == 1.0));
        break;
      case HighsVarType::kSemiContinuous:
      case HighsVarType::kSemiInteger:
        highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kError,
                     "Semicontinuous or semiinteger variables should have been "
                     "reformulated away before HighsMipSolverData::runSetup() "
                     "is called.");
        throw std::logic_error("Unexpected variable type");
    }
  }

  basisTransfer();

  numintegercols = integer_cols.size();
  detectSymmetries = detectSymmetries && numBin > 0;
  numCliqueEntriesAfterPresolve = cliquetable.getNumEntries();

  if (numRestarts == 0) {
    numCliqueEntriesAfterFirstPresolve = cliquetable.getNumEntries();
    highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kInfo,
                 // clang-format off
               "\nSolving MIP model with:\n"
               "   %" HIGHSINT_FORMAT " rows\n"
               "   %" HIGHSINT_FORMAT " cols (%" HIGHSINT_FORMAT" binary, %" HIGHSINT_FORMAT " integer, %" HIGHSINT_FORMAT" implied int., %" HIGHSINT_FORMAT " continuous)\n"
               "   %" HIGHSINT_FORMAT " nonzeros\n",
                 // clang-format on
                 mipsolver.numRow(), mipsolver.numCol(), numBin,
                 numintegercols - numBin, (HighsInt)implint_cols.size(),
                 (HighsInt)continuous_cols.size(), mipsolver.numNonzero());
  } else {
    highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kInfo,
                 "Model after restart has %" HIGHSINT_FORMAT
                 " rows, %" HIGHSINT_FORMAT " cols (%" HIGHSINT_FORMAT
                 " bin., %" HIGHSINT_FORMAT " int., %" HIGHSINT_FORMAT
                 " impl., %" HIGHSINT_FORMAT " cont.), and %" HIGHSINT_FORMAT
                 " nonzeros\n",
                 mipsolver.numRow(), mipsolver.numCol(), numBin,
                 numintegercols - numBin, (HighsInt)implint_cols.size(),
                 (HighsInt)continuous_cols.size(), mipsolver.numNonzero());
  }

  heuristics.setupIntCols();

#ifdef HIGHS_DEBUGSOL
  if (numRestarts == 0) {
    debugSolution.activate();
    assert(!debugSolution.debugSolActive ||
           checkSolution(debugSolution.debugSolution));
  }
#endif

  if (upper_limit == kHighsInf) analyticCenterComputed = false;
  analyticCenterStatus = HighsModelStatus::kNotset;
  analyticCenter.clear();

  symmetries.clear();

  if (numRestarts != 0)
    highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kInfo,
                 "\n");
  heuristics.initialiseLocalTrivialHeuristicsStatistics();
  heuristics.downCopyLocalTrivialHeuristicsStatistics(
      submip_trivial_heuristics_statistics_);
}

double HighsMipSolverData::transformNewIntegerFeasibleSolution(
    const std::vector<double>& sol,
    const bool possibly_store_as_new_incumbent) {
  HighsSolution solution;
  solution.col_value = sol;
  solution.value_valid = true;
  // Perform primal postsolve to get the original column values
  postSolveStack.undoPrimal(*mipsolver.options_mip_, solution);
  // Determine the row values, as they aren't computed in primal
  // postsolve
  HighsInt first_check_row =
      -1;  // mipsolver.mipdata_->presolve.debugGetCheckRow();
  HighsStatus return_status =
      calculateRowValuesQuad(*mipsolver.orig_model_, solution, first_check_row);
  if (kAllowDeveloperAssert) assert(return_status == HighsStatus::kOk);
  bool allow_try_again = true;
try_again:

  // compute the objective value in the original space
  double bound_violation_ = 0;
  double row_violation_ = 0;
  double integrality_violation_ = 0;

  // Compute to quad precision the objective function value of the MIP
  // being solved - including the offset, and independent of objective
  // sense
  //
  HighsCDouble mipsolver_quad_precision_objective_value =
      mipsolver.orig_model_->offset_;
  if (kAllowDeveloperAssert)
    assert((HighsInt)solution.col_value.size() ==
           mipsolver.orig_model_->num_col_);
  HighsInt check_col = -1;
  HighsInt check_int = -1;
  HighsInt check_row = -1;
  const bool debug_report = false;
  for (HighsInt i = 0; i != mipsolver.orig_model_->num_col_; ++i) {
    const double value = solution.col_value[i];
    mipsolver_quad_precision_objective_value +=
        mipsolver.orig_model_->col_cost_[i] * value;

    if (mipsolver.orig_model_->integrality_[i] == HighsVarType::kInteger) {
      double intval = std::floor(value + 0.5);
      double integrality_infeasibility = std::fabs(intval - value);
      if (integrality_infeasibility >
          mipsolver.options_mip_->mip_feasibility_tolerance) {
        if (debug_report)
          printf("Col %d[%s] value %g has integrality infeasibility %g\n",
                 int(i), mipsolver.orig_model_->col_names_[i].c_str(), value,
                 integrality_infeasibility);
        check_int = i;
      }
      integrality_violation_ =
          std::max(integrality_infeasibility, integrality_violation_);
    }

    const double lower = mipsolver.orig_model_->col_lower_[i];
    const double upper = mipsolver.orig_model_->col_upper_[i];
    double primal_infeasibility = 0;
    if (value < lower - mipsolver.options_mip_->mip_feasibility_tolerance) {
      primal_infeasibility = lower - value;
    } else if (value >
               upper + mipsolver.options_mip_->mip_feasibility_tolerance) {
      primal_infeasibility = value - upper;
    } else
      continue;
    if (primal_infeasibility >
        mipsolver.options_mip_->primal_feasibility_tolerance) {
      if (debug_report)
        printf("Col %d[%s] [%g, %g, %g] has infeasibility %g\n", int(i),
               mipsolver.orig_model_->col_names_[i].c_str(), lower, value,
               upper, primal_infeasibility);
      check_col = i;
    }
    bound_violation_ = std::max(bound_violation_, primal_infeasibility);
  }

  for (HighsInt i = 0; i != mipsolver.orig_model_->num_row_; ++i) {
    const double value = solution.row_value[i];
    const double lower = mipsolver.orig_model_->row_lower_[i];
    const double upper = mipsolver.orig_model_->row_upper_[i];
    double primal_infeasibility;
    if (value < lower - mipsolver.options_mip_->mip_feasibility_tolerance) {
      primal_infeasibility = lower - value;
    } else if (value >
               upper + mipsolver.options_mip_->mip_feasibility_tolerance) {
      primal_infeasibility = value - upper;
    } else
      continue;
    if (primal_infeasibility >
        mipsolver.options_mip_->primal_feasibility_tolerance) {
      if (debug_report)
        printf("Row %d[%s] [%g, %g, %g] has infeasibility %g\n", int(i),
               mipsolver.orig_model_->row_names_[i].c_str(), lower, value,
               upper, primal_infeasibility);
      check_row = i;
    }
    row_violation_ = std::max(row_violation_, primal_infeasibility);
  }

  bool feasible =
      bound_violation_ <= mipsolver.options_mip_->mip_feasibility_tolerance &&
      integrality_violation_ <=
          mipsolver.options_mip_->mip_feasibility_tolerance &&
      row_violation_ <= mipsolver.options_mip_->mip_feasibility_tolerance;

  if (!feasible && allow_try_again) {
    // printf(
    //     "trying to repair sol that is violated by %.12g bounds, %.12g "
    //     "integrality, %.12g rows\n",
    //     bound_violation_, integrality_violation_, row_violation_);
    HighsLp fixedModel = *mipsolver.orig_model_;
    fixedModel.integrality_.clear();
    for (HighsInt i = 0; i != mipsolver.orig_model_->num_col_; ++i) {
      if (mipsolver.orig_model_->integrality_[i] == HighsVarType::kInteger) {
        double solval = std::round(solution.col_value[i]);
        fixedModel.col_lower_[i] = std::max(fixedModel.col_lower_[i], solval);
        fixedModel.col_upper_[i] = std::min(fixedModel.col_upper_[i], solval);
      }
    }
    Highs tmpSolver;
    tmpSolver.setOptionValue("output_flag", false);
    tmpSolver.setOptionValue("simplex_scale_strategy", 0);
    tmpSolver.setOptionValue("presolve", "off");
    tmpSolver.setOptionValue("primal_feasibility_tolerance",
                             mipsolver.options_mip_->mip_feasibility_tolerance);
    tmpSolver.passModel(std::move(fixedModel));
    tmpSolver.run();

    if (tmpSolver.getInfo().primal_solution_status == kSolutionStatusFeasible) {
      solution = tmpSolver.getSolution();
      allow_try_again = false;
      goto try_again;
    }
  }

  // Get a double precision version of the objective function value of
  // the MIP being solved
  const double mipsolver_objective_value =
      double(mipsolver_quad_precision_objective_value);
  // Possible MIP solution callback
  if (!mipsolver.submip && feasible && mipsolver.callback_->user_callback &&
      mipsolver.callback_->active[kCallbackMipSolution]) {
    mipsolver.callback_->clearHighsCallbackDataOut();
    mipsolver.callback_->data_out.mip_solution = solution.col_value.data();
    const bool interrupt = interruptFromCallbackWithData(
        kCallbackMipSolution, mipsolver_objective_value, "Feasible solution");
    assert(!interrupt);
  }

  if (possibly_store_as_new_incumbent) {
    // Store the solution as incumbent in the original space if there
    // is no solution or if it is feasible
    if (feasible) {
      // if (!allow_try_again)
      //   printf("repaired solution with value %g\n",
      //   mipsolver_objective_value);
      // store
      mipsolver.row_violation_ = row_violation_;
      mipsolver.bound_violation_ = bound_violation_;
      mipsolver.integrality_violation_ = integrality_violation_;
      mipsolver.solution_ = std::move(solution.col_value);
      mipsolver.solution_objective_ = mipsolver_objective_value;
    } else {
      bool currentFeasible =
          mipsolver.solution_objective_ != kHighsInf &&
          mipsolver.bound_violation_ <=
              mipsolver.options_mip_->mip_feasibility_tolerance &&
          mipsolver.integrality_violation_ <=
              mipsolver.options_mip_->mip_feasibility_tolerance &&
          mipsolver.row_violation_ <=
              mipsolver.options_mip_->mip_feasibility_tolerance;
      //    check_col = 37;//mipsolver.mipdata_->presolve.debugGetCheckCol();
      //    check_row = 37;//mipsolver.mipdata_->presolve.debugGetCheckRow();
      std::string check_col_data = "";
      if (check_col >= 0) {
        check_col_data = " (col " + std::to_string(check_col);
        if (mipsolver.orig_model_->col_names_.size())
          check_col_data +=
              "[" + mipsolver.orig_model_->col_names_[check_col] + "]";
        check_col_data += ")";
      }
      std::string check_int_data = "";
      if (check_int >= 0) {
        check_int_data = " (col " + std::to_string(check_int);
        if (mipsolver.orig_model_->col_names_.size())
          check_int_data +=
              "[" + mipsolver.orig_model_->col_names_[check_int] + "]";
        check_int_data += ")";
      }
      std::string check_row_data = "";
      if (check_row >= 0) {
        check_row_data = " (row " + std::to_string(check_row);
        if (mipsolver.orig_model_->row_names_.size())
          check_row_data +=
              "[" + mipsolver.orig_model_->row_names_[check_row] + "]";
        check_row_data += ")";
      }
      highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kWarning,
                   //    printf(
                   "Solution with objective %g has untransformed violations: "
                   "bound = %.4g%s; integrality = %.4g%s; row = %.4g%s\n",
                   mipsolver_objective_value, bound_violation_,
                   check_col_data.c_str(), integrality_violation_,
                   check_int_data.c_str(), row_violation_,
                   check_row_data.c_str());

      const bool debug_repeat = false;  // true;//
      if (debug_repeat) {
        HighsSolution check_solution;
        check_solution.col_value = sol;
        check_solution.value_valid = true;
        postSolveStack.undoPrimal(*mipsolver.options_mip_, check_solution,
                                  check_col);
        fflush(stdout);
        if (kAllowDeveloperAssert) assert(111 == 999);
      }

      if (!currentFeasible) {
        // if the current incumbent is non existent or also not feasible we
        // still store the new one
        mipsolver.row_violation_ = row_violation_;
        mipsolver.bound_violation_ = bound_violation_;
        mipsolver.integrality_violation_ = integrality_violation_;
        mipsolver.solution_ = std::move(solution.col_value);
        mipsolver.solution_objective_ = mipsolver_objective_value;
      }

      // return infinity so that it is not used for bounding
      return kHighsInf;
    }
  }
  // return the objective value in the transformed space
  if (mipsolver.orig_model_->sense_ == ObjSense::kMaximize)
    return -double(mipsolver_quad_precision_objective_value +
                   mipsolver.model_->offset_);

  return double(mipsolver_quad_precision_objective_value -
                mipsolver.model_->offset_);
}

double HighsMipSolverData::percentageInactiveIntegers() const {
  return 100.0 * (1.0 - double(integer_cols.size() -
                               cliquetable.getSubstitutions().size()) /
                            numintegercols);
}

void HighsMipSolverData::performRestart() {
  HighsBasis root_basis;
  HighsPseudocostInitialization pscostinit(
      pseudocost, mipsolver.options_mip_->mip_pscost_minreliable,
      postSolveStack);

  mipsolver.pscostinit = &pscostinit;
  ++numRestarts;
  num_leaves_before_run = num_leaves;
  num_nodes_before_run = num_nodes;
  num_nodes_before_run = num_nodes;
  total_lp_iterations_before_run = total_lp_iterations;
  heuristic_lp_iterations_before_run = heuristic_lp_iterations;
  sepa_lp_iterations_before_run = sepa_lp_iterations;
  sb_lp_iterations_before_run = sb_lp_iterations;
  HighsInt numLpRows = lp.getLp().num_row_;
  HighsInt numModelRows = mipsolver.numRow();
  HighsInt numCuts = numLpRows - numModelRows;
  if (numCuts > 0) postSolveStack.appendCutsToModel(numCuts);
  auto integrality = std::move(presolvedModel.integrality_);
  double offset = presolvedModel.offset_;
  presolvedModel = lp.getLp();
  presolvedModel.offset_ = offset;
  presolvedModel.integrality_ = std::move(integrality);

  const HighsBasis& basis = firstrootbasis;
  if (basis.valid) {
    // if we have a basis after solving the root LP, we expand it to the
    // original space so that it can be used for constructing a starting basis
    // for the presolved model after the restart
    root_basis.col_status.resize(postSolveStack.getOrigNumCol());
    root_basis.row_status.resize(postSolveStack.getOrigNumRow(),
                                 HighsBasisStatus::kBasic);
    root_basis.valid = true;

    for (HighsInt i = 0; i < mipsolver.model_->num_col_; ++i)
      root_basis.col_status[postSolveStack.getOrigColIndex(i)] =
          basis.col_status[i];

    HighsInt numRow = basis.row_status.size();
    for (HighsInt i = 0; i < numRow; ++i)
      root_basis.row_status[postSolveStack.getOrigRowIndex(i)] =
          basis.row_status[i];

    mipsolver.rootbasis = &root_basis;
  }

  // transform the objective upper bound into the original space, as it is
  // expected during presolve
  upper_limit += mipsolver.model_->offset_;
  optimality_limit += mipsolver.model_->offset_;
  upper_bound += mipsolver.model_->offset_;
  lower_bound += mipsolver.model_->offset_;

  // remove the current incumbent. Any incumbent is already transformed into the
  // original space and kept there
  incumbent.clear();
  pruned_treeweight = 0;
  nodequeue.clear();
  globalOrbits.reset();

  runPresolve();

  if (mipsolver.modelstatus_ != HighsModelStatus::kNotset) {
    // transform the objective limit to the current model
    upper_limit -= mipsolver.model_->offset_;
    optimality_limit -= mipsolver.model_->offset_;

    if (mipsolver.modelstatus_ == HighsModelStatus::kOptimal) {
      mipsolver.mipdata_->upper_bound = 0;
      mipsolver.mipdata_->transformNewIntegerFeasibleSolution(
          std::vector<double>());
    } else
      upper_bound -= mipsolver.model_->offset_;

    lower_bound = upper_bound;
    if (mipsolver.solution_objective_ != kHighsInf &&
        mipsolver.modelstatus_ == HighsModelStatus::kInfeasible)
      mipsolver.modelstatus_ = HighsModelStatus::kOptimal;
    return;
  }
  runSetup();

  postSolveStack.removeCutsFromModel(numCuts);

  // HighsNodeQueue oldNodeQueue;
  // std::swap(nodequeue, oldNodeQueue);

  // remove the pointer into the stack-space of this function
  if (mipsolver.rootbasis == &root_basis) mipsolver.rootbasis = nullptr;
  mipsolver.pscostinit = nullptr;
}

void HighsMipSolverData::basisTransfer() {
  // if a root basis is given, construct a basis for the root LP from
  // in the reduced problem space after presolving
  if (mipsolver.rootbasis) {
    const HighsInt numRow = mipsolver.numRow();
    const HighsInt numCol = mipsolver.numCol();
    firstrootbasis.col_status.assign(numCol, HighsBasisStatus::kNonbasic);
    firstrootbasis.row_status.assign(numRow, HighsBasisStatus::kNonbasic);
    firstrootbasis.valid = true;
    firstrootbasis.alien = true;

    for (HighsInt i = 0; i < numRow; ++i) {
      HighsBasisStatus status =
          mipsolver.rootbasis->row_status[postSolveStack.getOrigRowIndex(i)];
      firstrootbasis.row_status[i] = status;
    }

    for (HighsInt i = 0; i < numCol; ++i) {
      HighsBasisStatus status =
          mipsolver.rootbasis->col_status[postSolveStack.getOrigColIndex(i)];
      firstrootbasis.col_status[i] = status;
    }
  }
}

const std::vector<double>& HighsMipSolverData::getSolution() const {
  return incumbent;
}

bool HighsMipSolverData::addIncumbent(const std::vector<double>& sol,
                                      double solobj,
                                      const char solution_source) {
  const bool execute_mip_solution_callback =
      !mipsolver.submip &&
      (mipsolver.callback_->user_callback
           ? mipsolver.callback_->active[kCallbackMipSolution]
           : false);
  // Determine whether the potential new incumbent should be
  // transformed
  //
  // Happens if solobj improves on the upper bound or the MIP solution
  // callback is active
  const bool possibly_store_as_new_incumbent = solobj < upper_bound;
  const bool get_transformed_solution =
      possibly_store_as_new_incumbent || execute_mip_solution_callback;
  // Get the transformed objective and solution if required
  const double transformed_solobj =
      get_transformed_solution ? transformNewIntegerFeasibleSolution(
                                     sol, possibly_store_as_new_incumbent)
                               : 0;

  if (possibly_store_as_new_incumbent) {
    // #1463 use pre-computed transformed_solobj
    solobj = transformed_solobj;
    //    solobj = transformNewIntegerFeasibleSolution(sol);

    if (solobj >= upper_bound) return false;
    upper_bound = solobj;
    incumbent = sol;
    double new_upper_limit = computeNewUpperLimit(solobj, 0.0, 0.0);

    if (!mipsolver.submip) saveReportMipSolution(new_upper_limit);
    if (new_upper_limit < upper_limit) {
      ++numImprovingSols;
      upper_limit = new_upper_limit;
      optimality_limit =
          computeNewUpperLimit(solobj, mipsolver.options_mip_->mip_abs_gap,
                               mipsolver.options_mip_->mip_rel_gap);
      nodequeue.setOptimalityLimit(optimality_limit);
      debugSolution.newIncumbentFound();
      domain.propagate();
      if (!domain.infeasible()) redcostfixing.propagateRootRedcost(mipsolver);

      if (domain.infeasible()) {
        pruned_treeweight = 1.0;
        nodequeue.clear();
        return true;
      }
      cliquetable.extractObjCliques(mipsolver);
      if (domain.infeasible()) {
        pruned_treeweight = 1.0;
        nodequeue.clear();
        return true;
      }
      pruned_treeweight += nodequeue.performBounding(upper_limit);
      printDisplayLine(solution_source);
    }
  } else if (incumbent.empty())
    incumbent = sol;

  return true;
}

static std::array<char, 22> convertToPrintString(int64_t val) {
  double l = std::log10(std::max(1.0, double(val)));
  std::array<char, 22> printString;
  switch (int(l)) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
      std::snprintf(printString.data(), 22, "%" PRId64, val);
      break;
    case 6:
    case 7:
    case 8:
      std::snprintf(printString.data(), 22, "%" PRId64 "k", val / 1000);
      break;
    default:
      std::snprintf(printString.data(), 22, "%" PRId64 "m", val / 1000000);
  }

  return printString;
}

static std::array<char, 22> convertToPrintString(double val,
                                                 const char* trailingStr = "") {
  std::array<char, 22> printString;
  double l = std::abs(val) == kHighsInf
                 ? 0.0
                 : std::log10(std::max(1e-6, std::abs(val)));
  switch (int(l)) {
    case 0:
    case 1:
    case 2:
    case 3:
      std::snprintf(printString.data(), 22, "%.10g%s", val, trailingStr);
      break;
    case 4:
      std::snprintf(printString.data(), 22, "%.11g%s", val, trailingStr);
      break;
    case 5:
      std::snprintf(printString.data(), 22, "%.12g%s", val, trailingStr);
      break;
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
      std::snprintf(printString.data(), 22, "%.13g%s", val, trailingStr);
      break;
    default:
      std::snprintf(printString.data(), 22, "%.9g%s", val, trailingStr);
  }

  return printString;
}

void HighsMipSolverData::printDisplayLine(const char solution_source) {
  // MIP logging method
  //
  // Note that if the original problem is a maximization, the cost
  // coefficients are negated so that the MIP solver only solves a
  // minimization. Hence, in preparing to print the display line, the
  // dual bound (lb) is always less than the primal bound (ub). When
  // printed, the sense of the optimization is applied so that the
  // values printed correspond to the original objective.

  // No point in computing all the logging values if logging is off
  bool output_flag = *mipsolver.options_mip_->log_options.output_flag;
  if (!output_flag) return;

  double time = mipsolver.timer_.read(mipsolver.timer_.solve_clock);
  if (solution_source == ' ' &&
      time - last_disptime < mipsolver.options_mip_->mip_min_logging_interval)
    return;
  last_disptime = time;

  if (num_disp_lines % 20 == 0) {
    highsLogUser(
        mipsolver.options_mip_->log_options, HighsLogType::kInfo,
        // clang-format off
	"\nSrc: B => Branching; C => Central rounding; F => Feasibility pump; H => Heuristic; L => Sub-MIP\nSrc: P => Empty MIP; R => Randomized rounding; S => Solve LP; T => Evaluate node; U => Unbounded\n" 
        "\n        Nodes      |    B&B Tree     |            Objective Bounds              |  Dynamic Constraints |       Work      \n"
          "Src  Proc. InQueue |  Leaves   Expl. | BestBound       BestSol              Gap |   Cuts   InLp Confl. | LpIters     Time\n\n"
        // clang-format on
    );

    //"   %7s | %10s | %10s | %10s | %10s | %-15s | %-15s | %7s | %7s "
    //"| %8s | %8s\n",
    //"time", "open nodes", "nodes", "leaves", "lpiters", "dual bound",
    //"primal bound", "cutpool", "confl.", "gap", "explored");
  }

  ++num_disp_lines;

  std::array<char, 22> print_nodes = convertToPrintString(num_nodes);
  std::array<char, 22> queue_nodes =
      convertToPrintString(nodequeue.numActiveNodes());
  std::array<char, 22> print_leaves =
      convertToPrintString(num_leaves - num_leaves_before_run);

  double explored = 100 * double(pruned_treeweight);

  double offset = mipsolver.model_->offset_;
  double lb = lower_bound + offset;
  if (std::abs(lb) <= epsilon) lb = 0;
  double ub = kHighsInf;
  double gap = kHighsInf;

  std::array<char, 22> print_lp_iters =
      convertToPrintString(total_lp_iterations);
  if (upper_bound != kHighsInf) {
    ub = upper_bound + offset;

    if (std::fabs(ub) <= epsilon) ub = 0;
    lb = std::min(ub, lb);
    if (ub == 0.0)
      gap = lb == 0.0 ? 0.0 : kHighsInf;
    else
      gap = 100. * (ub - lb) / fabs(ub);

    std::array<char, 22> gap_string;
    if (gap >= 9999.)
      std::strcpy(gap_string.data(), "Large");
    else
      std::snprintf(gap_string.data(), gap_string.size(), "%.2f%%", gap);

    std::array<char, 22> ub_string;
    if (mipsolver.options_mip_->objective_bound < ub) {
      ub = mipsolver.options_mip_->objective_bound;
      ub_string =
          convertToPrintString((int)mipsolver.orig_model_->sense_ * ub, "*");
    } else
      ub_string = convertToPrintString((int)mipsolver.orig_model_->sense_ * ub);

    std::array<char, 22> lb_string =
        convertToPrintString((int)mipsolver.orig_model_->sense_ * lb);

    highsLogUser(
        mipsolver.options_mip_->log_options, HighsLogType::kInfo,
        // clang-format off
                 " %c %7s %7s   %7s %6.2f%%   %-15s %-15s %8s   %6" HIGHSINT_FORMAT " %6" HIGHSINT_FORMAT " %6" HIGHSINT_FORMAT "   %7s %7.1fs\n",
        // clang-format on
        solution_source, print_nodes.data(), queue_nodes.data(),
        print_leaves.data(), explored, lb_string.data(), ub_string.data(),
        gap_string.data(), cutpool.getNumCuts(),
        lp.numRows() - lp.getNumModelRows(), conflictPool.getNumConflicts(),
        print_lp_iters.data(), time);
  } else {
    std::array<char, 22> ub_string;
    if (mipsolver.options_mip_->objective_bound < ub) {
      ub = mipsolver.options_mip_->objective_bound;
      ub_string =
          convertToPrintString((int)mipsolver.orig_model_->sense_ * ub, "*");
    } else
      ub_string = convertToPrintString((int)mipsolver.orig_model_->sense_ * ub);

    std::array<char, 22> lb_string =
        convertToPrintString((int)mipsolver.orig_model_->sense_ * lb);

    highsLogUser(
        mipsolver.options_mip_->log_options, HighsLogType::kInfo,
        // clang-format off
        " %c %7s %7s   %7s %6.2f%%   %-15s %-15s %8.2f   %6" HIGHSINT_FORMAT " %6" HIGHSINT_FORMAT " %6" HIGHSINT_FORMAT "   %7s %7.1fs\n",
        // clang-format on
        solution_source, print_nodes.data(), queue_nodes.data(),
        print_leaves.data(), explored, lb_string.data(), ub_string.data(), gap,
        cutpool.getNumCuts(), lp.numRows() - lp.getNumModelRows(),
        conflictPool.getNumConflicts(), print_lp_iters.data(), time);
  }
  // Check that limitsToBounds yields the same values for the
  // dual_bound, primal_bound (modulo optimization sense) and
  // mip_rel_gap
  double dual_bound;
  double primal_bound;
  double mip_rel_gap;
  limitsToBounds(dual_bound, primal_bound, mip_rel_gap);
  assert(dual_bound == (int)mipsolver.orig_model_->sense_ * lb);
  assert(primal_bound == (int)mipsolver.orig_model_->sense_ * ub);
  assert(mip_rel_gap == gap);
  // Possibly interrupt from MIP logging callback
  mipsolver.callback_->clearHighsCallbackDataOut();
  const bool interrupt = interruptFromCallbackWithData(
      kCallbackMipLogging, mipsolver.solution_objective_, "MIP logging");
  assert(!interrupt);
}

bool HighsMipSolverData::rootSeparationRound(
    HighsSeparation& sepa, HighsInt& ncuts, HighsLpRelaxation::Status& status) {
  int64_t tmpLpIters = -lp.getNumLpIterations();
  ncuts = sepa.separationRound(domain, status);
  tmpLpIters += lp.getNumLpIterations();
  avgrootlpiters = lp.getAvgSolveIters();
  total_lp_iterations += tmpLpIters;
  sepa_lp_iterations += tmpLpIters;

  status = evaluateRootLp();
  if (status == HighsLpRelaxation::Status::kInfeasible) return true;

  const std::vector<double>& solvals = lp.getLpSolver().getSolution().col_value;

  if (mipsolver.submip || incumbent.empty()) {
    heuristics.randomizedRounding(solvals);
    heuristics.flushStatistics();
    status = evaluateRootLp();
    if (status == HighsLpRelaxation::Status::kInfeasible) return true;
  }

  return false;
}

HighsLpRelaxation::Status HighsMipSolverData::evaluateRootLp() {
  do {
    domain.propagate();

    if (globalOrbits && !domain.infeasible())
      globalOrbits->orbitalFixing(domain);

    if (domain.infeasible()) {
      lower_bound = std::min(kHighsInf, upper_bound);
      pruned_treeweight = 1.0;
      num_nodes += 1;
      num_leaves += 1;
      return HighsLpRelaxation::Status::kInfeasible;
    }

    bool lpBoundsChanged = false;
    if (!domain.getChangedCols().empty()) {
      lpBoundsChanged = true;
      removeFixedIndices();
      lp.flushDomain(domain);
    }

    bool lpWasSolved = false;
    HighsLpRelaxation::Status status;
    if (lpBoundsChanged ||
        lp.getLpSolver().getModelStatus() == HighsModelStatus::kNotset) {
      int64_t lpIters = -lp.getNumLpIterations();
      status = lp.resolveLp(&domain);
      lpIters += lp.getNumLpIterations();
      total_lp_iterations += lpIters;
      avgrootlpiters = lp.getAvgSolveIters();
      lpWasSolved = true;

      if (status == HighsLpRelaxation::Status::kUnbounded) {
        if (mipsolver.solution_.empty())
          mipsolver.modelstatus_ = HighsModelStatus::kUnboundedOrInfeasible;
        else
          mipsolver.modelstatus_ = HighsModelStatus::kUnbounded;

        pruned_treeweight = 1.0;
        num_nodes += 1;
        num_leaves += 1;
        return status;
      }

      if (status == HighsLpRelaxation::Status::kOptimal &&
          lp.getFractionalIntegers().empty() &&
          addIncumbent(lp.getLpSolver().getSolution().col_value,
                       lp.getObjective(), kSolutionSourceEvaluateNode)) {
        mipsolver.modelstatus_ = HighsModelStatus::kOptimal;
        lower_bound = upper_bound;
        pruned_treeweight = 1.0;
        num_nodes += 1;
        num_leaves += 1;
        return HighsLpRelaxation::Status::kInfeasible;
      }
    } else
      status = lp.getStatus();

    if (status == HighsLpRelaxation::Status::kInfeasible) {
      lower_bound = std::min(kHighsInf, upper_bound);
      pruned_treeweight = 1.0;
      num_nodes += 1;
      num_leaves += 1;
      return status;
    }

    if (lp.unscaledDualFeasible(lp.getStatus())) {
      lower_bound = std::max(lp.getObjective(), lower_bound);
      if (lpWasSolved) {
        redcostfixing.addRootRedcost(mipsolver,
                                     lp.getLpSolver().getSolution().col_dual,
                                     lp.getObjective());
        if (upper_limit != kHighsInf)
          redcostfixing.propagateRootRedcost(mipsolver);
      }
    }

    if (lower_bound > optimality_limit) {
      pruned_treeweight = 1.0;
      num_nodes += 1;
      num_leaves += 1;
      return HighsLpRelaxation::Status::kInfeasible;
    }

    if (domain.getChangedCols().empty()) return status;
  } while (true);
}

void HighsMipSolverData::evaluateRootNode() {
  HighsInt maxSepaRounds = mipsolver.submip ? 5 : kHighsIInf;
  if (numRestarts == 0)
    maxSepaRounds =
        std::min(HighsInt(2 * std::sqrt(maxTreeSizeLog2)), maxSepaRounds);
  std::unique_ptr<SymmetryDetectionData> symData;
  highs::parallel::TaskGroup tg;
restart:
  if (detectSymmetries) startSymmetryDetection(tg, symData);
  if (!analyticCenterComputed) startAnalyticCenterComputation(tg);

  // lp.getLpSolver().setOptionValue(
  //     "dual_simplex_cost_perturbation_multiplier", 10.0);
  lp.setIterationLimit();
  lp.loadModel();
  domain.clearChangedCols();
  lp.setObjectiveLimit(upper_limit);
  lower_bound = std::max(lower_bound, domain.getObjectiveLowerBound());

  printDisplayLine();

  if (firstrootbasis.valid)
    lp.getLpSolver().setBasis(firstrootbasis,
                              "HighsMipSolverData::evaluateRootNode");
  else
    lp.getLpSolver().setOptionValue("presolve", "on");
  if (mipsolver.options_mip_->highs_debug_level)
    lp.getLpSolver().setOptionValue("output_flag",
                                    mipsolver.options_mip_->output_flag);
  //  lp.getLpSolver().setOptionValue("log_dev_level", kHighsLogDevLevelInfo);
  //  lp.getLpSolver().setOptionValue("log_file",
  //  mipsolver.options_mip_->log_file);
  HighsLpRelaxation::Status status = evaluateRootLp();
  if (numRestarts == 0) firstrootlpiters = total_lp_iterations;

  lp.getLpSolver().setOptionValue("output_flag", false);
  lp.getLpSolver().setOptionValue("presolve", "off");
  lp.getLpSolver().setOptionValue("parallel", "off");

  if (status == HighsLpRelaxation::Status::kInfeasible ||
      status == HighsLpRelaxation::Status::kUnbounded)
    return;

  firstlpsol = lp.getSolution().col_value;
  firstlpsolobj = lp.getObjective();
  rootlpsolobj = firstlpsolobj;

  if (lp.getLpSolver().getBasis().valid && lp.numRows() == mipsolver.numRow())
    firstrootbasis = lp.getLpSolver().getBasis();
  else {
    // the root basis is later expected to be consistent for the model without
    // cuts so set it to the slack basis if the current basis already includes
    // cuts, e.g. due to a restart
    firstrootbasis.col_status.assign(mipsolver.numCol(),
                                     HighsBasisStatus::kNonbasic);
    firstrootbasis.row_status.assign(mipsolver.numRow(),
                                     HighsBasisStatus::kBasic);
    firstrootbasis.valid = true;
  }

  if (cutpool.getNumCuts() != 0) {
    assert(numRestarts != 0);
    HighsCutSet cutset;
    cutpool.separateLpCutsAfterRestart(cutset);
#ifdef HIGHS_DEBUGSOL
    for (HighsInt i = 0; i < cutset.numCuts(); ++i) {
      debugSolution.checkCut(cutset.ARindex_.data() + cutset.ARstart_[i],
                             cutset.ARvalue_.data() + cutset.ARstart_[i],
                             cutset.ARstart_[i + 1] - cutset.ARstart_[i],
                             cutset.upper_[i]);
    }
#endif
    lp.addCuts(cutset);
    status = evaluateRootLp();
    lp.removeObsoleteRows();
    if (status == HighsLpRelaxation::Status::kInfeasible) return;
  }

  lp.setIterationLimit(std::max(10000, int(10 * avgrootlpiters)));

  // make sure first line after solving root LP is printed
  last_disptime = -kHighsInf;

  heuristics.randomizedRounding(firstlpsol);
  heuristics.flushStatistics();

  status = evaluateRootLp();
  if (status == HighsLpRelaxation::Status::kInfeasible) return;

  rootlpsolobj = firstlpsolobj;
  removeFixedIndices();
  if (mipsolver.options_mip_->presolve != kHighsOffString) {
    double fixingRate = percentageInactiveIntegers();
    if (fixingRate >= 10.0) {
      tg.cancel();
      highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kInfo,
                   "\n%.1f%% inactive integer columns, restarting\n",
                   fixingRate);
      tg.taskWait();
      performRestart();
      ++numRestartsRoot;
      if (mipsolver.modelstatus_ == HighsModelStatus::kNotset) goto restart;

      return;
    }
  }

  // begin separation
  std::vector<double> avgdirection;
  std::vector<double> curdirection;
  avgdirection.resize(mipsolver.numCol());
  curdirection.resize(mipsolver.numCol());

  HighsInt stall = 0;
  double smoothprogress = 0.0;
  HighsInt nseparounds = 0;
  HighsSeparation sepa(mipsolver);
  sepa.setLpRelaxation(&lp);

  while (lp.scaledOptimal(status) && !lp.getFractionalIntegers().empty() &&
         stall < 3) {
    printDisplayLine();

    if (checkLimits()) return;

    if (nseparounds == maxSepaRounds) break;

    removeFixedIndices();

    if (!mipsolver.submip &&
        mipsolver.options_mip_->presolve != kHighsOffString) {
      double fixingRate = percentageInactiveIntegers();
      if (fixingRate >= 10.0) {
        stall = -1;
        break;
      }
    }

    ++nseparounds;

    HighsInt ncuts;
    if (rootSeparationRound(sepa, ncuts, status)) return;
    if (nseparounds >= 5 && !mipsolver.submip && !analyticCenterComputed) {
      if (checkLimits()) return;
      finishAnalyticCenterComputation(tg);
      heuristics.centralRounding();
      heuristics.flushStatistics();

      if (checkLimits()) return;
      status = evaluateRootLp();
      if (status == HighsLpRelaxation::Status::kInfeasible) return;
    }

    HighsCDouble sqrnorm = 0.0;
    const auto& solvals = lp.getSolution().col_value;

    for (HighsInt i = 0; i != mipsolver.numCol(); ++i) {
      curdirection[i] = firstlpsol[i] - solvals[i];

      // if (mip.integrality_[i] == 2 && lp.getObjective() > firstobj &&
      //    std::abs(curdirection[i]) > 1e-6)
      //  pseudocost.addObservation(i, -curdirection[i],
      //                            lp.getObjective() - firstobj);

      sqrnorm += curdirection[i] * curdirection[i];
    }
#if 1
    double scale = double(1.0 / sqrt(sqrnorm));
    sqrnorm = 0.0;
    HighsCDouble dotproduct = 0.0;
    for (HighsInt i = 0; i != mipsolver.numCol(); ++i) {
      avgdirection[i] =
          (scale * curdirection[i] - avgdirection[i]) / nseparounds;
      sqrnorm += avgdirection[i] * avgdirection[i];
      dotproduct += avgdirection[i] * curdirection[i];
    }
#endif

    double progress = double(dotproduct / sqrt(sqrnorm));

    if (nseparounds == 1) {
      smoothprogress = progress;
    } else {
      double alpha = 1.0 / 3.0;
      double nextprogress = (1.0 - alpha) * smoothprogress + alpha * progress;

      if (nextprogress < smoothprogress * 1.01 &&
          (lp.getObjective() - firstlpsolobj) <=
              (rootlpsolobj - firstlpsolobj) * 1.001)
        ++stall;
      else {
        stall = 0;
      }
      smoothprogress = nextprogress;
    }

    rootlpsolobj = lp.getObjective();
    lp.setIterationLimit(std::max(10000, int(10 * avgrootlpiters)));
    if (ncuts == 0) break;
  }

  lp.setIterationLimit();
  status = evaluateRootLp();
  if (status == HighsLpRelaxation::Status::kInfeasible) return;

  rootlpsol = lp.getLpSolver().getSolution().col_value;
  rootlpsolobj = lp.getObjective();
  lp.setIterationLimit(std::max(10000, int(10 * avgrootlpiters)));

  if (!analyticCenterComputed) {
    if (checkLimits()) return;
    finishAnalyticCenterComputation(tg);
    heuristics.centralRounding();
    heuristics.flushStatistics();

    // if there are new global bound changes we reevaluate the LP and do one
    // more separation round
    if (checkLimits()) return;
    bool separate = !domain.getChangedCols().empty();
    status = evaluateRootLp();
    if (status == HighsLpRelaxation::Status::kInfeasible) return;
    if (separate && lp.scaledOptimal(status)) {
      HighsInt ncuts;
      if (rootSeparationRound(sepa, ncuts, status)) return;
      ++nseparounds;
      printDisplayLine();
    }
  }

  printDisplayLine();
  if (checkLimits()) return;

  do {
    if (rootlpsol.empty()) break;
    if (upper_limit != kHighsInf && !moreHeuristicsAllowed()) break;

    heuristics.rootReducedCost();
    heuristics.flushStatistics();

    if (checkLimits()) return;

    // if there are new global bound changes we reevaluate the LP and do one
    // more separation round
    bool separate = !domain.getChangedCols().empty();
    status = evaluateRootLp();
    if (status == HighsLpRelaxation::Status::kInfeasible) return;
    if (separate && lp.scaledOptimal(status)) {
      HighsInt ncuts;
      if (rootSeparationRound(sepa, ncuts, status)) return;

      ++nseparounds;
      printDisplayLine();
    }

    if (upper_limit != kHighsInf && !moreHeuristicsAllowed()) break;

    if (checkLimits()) return;
    heuristics.RENS(rootlpsol);
    heuristics.flushStatistics();

    if (checkLimits()) return;
    // if there are new global bound changes we reevaluate the LP and do one
    // more separation round
    separate = !domain.getChangedCols().empty();
    status = evaluateRootLp();
    if (status == HighsLpRelaxation::Status::kInfeasible) return;
    if (separate && lp.scaledOptimal(status)) {
      HighsInt ncuts;
      if (rootSeparationRound(sepa, ncuts, status)) return;

      ++nseparounds;

      printDisplayLine();
    }
    if (checkLimits()) return;

    // --->
    // End of primal heuristics, unless not a sub-MIP, and no feasible
    // point found
    //
    if (mipsolver.options_mip_->mip_trivial_heuristics != kHighsOffString) {
      // Try trivial heuristics
      heuristics.trivial();
      heuristics.flushStatistics();
    }
    // <---

    if (upper_limit != kHighsInf || mipsolver.submip) break;

    if (checkLimits()) return;
    heuristics.feasibilityPump();
    heuristics.flushStatistics();

    if (checkLimits()) return;
    status = evaluateRootLp();
    if (status == HighsLpRelaxation::Status::kInfeasible) return;
  } while (false);

  if (lower_bound > upper_limit) {
    mipsolver.modelstatus_ = HighsModelStatus::kOptimal;
    pruned_treeweight = 1.0;
    num_nodes += 1;
    num_leaves += 1;
    return;
  }

  // if there are new global bound changes we reevaluate the LP and do one
  // more separation round
  bool separate = !domain.getChangedCols().empty();
  status = evaluateRootLp();
  if (status == HighsLpRelaxation::Status::kInfeasible) return;
  if (separate && lp.scaledOptimal(status)) {
    HighsInt ncuts;
    if (rootSeparationRound(sepa, ncuts, status)) return;

    ++nseparounds;
    printDisplayLine();
  }

  removeFixedIndices();
  if (lp.getLpSolver().getBasis().valid) lp.removeObsoleteRows();
  rootlpsolobj = lp.getObjective();

  printDisplayLine();

  if (lower_bound <= upper_limit) {
    if (!mipsolver.submip &&
        mipsolver.options_mip_->presolve != kHighsOffString) {
      if (!analyticCenterComputed) finishAnalyticCenterComputation(tg);
      double fixingRate = percentageInactiveIntegers();
      if (fixingRate >= 2.5 + 7.5 * mipsolver.submip ||
          (!mipsolver.submip && fixingRate > 0 && numRestarts == 0)) {
        tg.cancel();
        highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kInfo,
                     "\n%.1f%% inactive integer columns, restarting\n",
                     fixingRate);
        if (stall != -1) maxSepaRounds = std::min(maxSepaRounds, nseparounds);
        tg.taskWait();
        performRestart();
        ++numRestartsRoot;
        if (mipsolver.modelstatus_ == HighsModelStatus::kNotset) goto restart;

        return;
      }
    }

    if (detectSymmetries) {
      finishSymmetryDetection(tg, symData);
      status = evaluateRootLp();
      if (status == HighsLpRelaxation::Status::kInfeasible) return;
    }

    // add the root node to the nodequeue to initialize the search
    nodequeue.emplaceNode(std::vector<HighsDomainChange>(),
                          std::vector<HighsInt>(), lower_bound,
                          lp.computeBestEstimate(pseudocost), 1);
  }
}

bool HighsMipSolverData::checkLimits(int64_t nodeOffset) const {
  const HighsOptions& options = *mipsolver.options_mip_;

  // Possible user interrupt
  if (!mipsolver.submip && mipsolver.callback_->user_callback) {
    mipsolver.callback_->clearHighsCallbackDataOut();
    if (interruptFromCallbackWithData(kCallbackMipInterrupt,
                                      mipsolver.solution_objective_,
                                      "MIP check limits")) {
      if (mipsolver.modelstatus_ == HighsModelStatus::kNotset) {
        highsLogDev(options.log_options, HighsLogType::kInfo,
                    "User interrupt\n");
        mipsolver.modelstatus_ = HighsModelStatus::kInterrupt;
      }
      return true;
    }
  }
  // Possible termination due to objective being at least as good as
  // the target value
  if (!mipsolver.submip && mipsolver.solution_objective_ < kHighsInf &&
      options.objective_target > -kHighsInf) {
    // Note:
    //
    // Whether the sense is ObjSense::kMinimize or
    // ObjSense::kMaximize, the undefined value of
    // mipsolver.solution_objective_ is kHighsInf, and the default
    // target value is -kHighsInf, so had to rule out these cases in
    // the conditional statement above.
    //
    // mipsolver.solution_objective_ is the actual objective of the
    // MIP - including the offset, and independent of objective sense
    //
    // The target is reached if the objective is below (above) the
    // target value when minimizing (maximizing).
    const int int_sense = int(this->mipsolver.orig_model_->sense_);
    const bool reached_objective_target =
        int_sense * mipsolver.solution_objective_ <
        int_sense * options.objective_target;
    if (reached_objective_target) {
      if (mipsolver.modelstatus_ == HighsModelStatus::kNotset) {
        highsLogDev(options.log_options, HighsLogType::kInfo,
                    "Reached objective target\n");
        mipsolver.modelstatus_ = HighsModelStatus::kObjectiveTarget;
      }
      return true;
    }
  }

  if (options.mip_max_nodes != kHighsIInf &&
      num_nodes + nodeOffset >= options.mip_max_nodes) {
    if (mipsolver.modelstatus_ == HighsModelStatus::kNotset) {
      highsLogDev(options.log_options, HighsLogType::kInfo,
                  "Reached node limit\n");
      mipsolver.modelstatus_ = HighsModelStatus::kSolutionLimit;
    }
    return true;
  }

  if (options.mip_max_leaves != kHighsIInf &&
      num_leaves >= options.mip_max_leaves) {
    if (mipsolver.modelstatus_ == HighsModelStatus::kNotset) {
      highsLogDev(options.log_options, HighsLogType::kInfo,
                  "Reached leaf node limit\n");
      mipsolver.modelstatus_ = HighsModelStatus::kSolutionLimit;
    }
    return true;
  }

  if (options.mip_max_improving_sols != kHighsIInf &&
      numImprovingSols >= options.mip_max_improving_sols) {
    if (mipsolver.modelstatus_ == HighsModelStatus::kNotset) {
      highsLogDev(options.log_options, HighsLogType::kInfo,
                  "Reached improving solution limit\n");
      mipsolver.modelstatus_ = HighsModelStatus::kSolutionLimit;
    }
    return true;
  }

  if (mipsolver.timer_.read(mipsolver.timer_.solve_clock) >=
      options.time_limit) {
    if (mipsolver.modelstatus_ == HighsModelStatus::kNotset) {
      highsLogDev(options.log_options, HighsLogType::kInfo,
                  "Reached time limit\n");
      mipsolver.modelstatus_ = HighsModelStatus::kTimeLimit;
    }
    return true;
  }

  return false;
}

void HighsMipSolverData::checkObjIntegrality() {
  objectiveFunction.checkIntegrality(epsilon);
  if (objectiveFunction.isIntegral() && numRestarts == 0) {
    highsLogUser(mipsolver.options_mip_->log_options, HighsLogType::kInfo,
                 "Objective function is integral with scale %g\n",
                 objectiveFunction.integralScale());
  }
}

void HighsMipSolverData::setupDomainPropagation() {
  const HighsLp& model = *mipsolver.model_;
  highsSparseTranspose(model.num_row_, model.num_col_, model.a_matrix_.start_,
                       model.a_matrix_.index_, model.a_matrix_.value_, ARstart_,
                       ARindex_, ARvalue_);

  pseudocost = HighsPseudocost(mipsolver);

  // compute the maximal absolute coefficients to filter propagation
  maxAbsRowCoef.resize(mipsolver.model_->num_row_);
  for (HighsInt i = 0; i != mipsolver.model_->num_row_; ++i) {
    double maxabsval = 0.0;

    HighsInt start = ARstart_[i];
    HighsInt end = ARstart_[i + 1];
    for (HighsInt j = start; j != end; ++j)
      maxabsval = std::max(maxabsval, std::abs(ARvalue_[j]));

    maxAbsRowCoef[i] = maxabsval;
  }

  domain = HighsDomain(mipsolver);
  domain.computeRowActivities();
}

void HighsMipSolverData::saveReportMipSolution(const double new_upper_limit) {
  const bool non_improving = new_upper_limit >= upper_limit;
  if (mipsolver.submip) return;
  if (non_improving) return;

  /*
  printf(
      "%7s dimension(%d, %d) "
      "%4simproving solution: numImprovingSols = %4d; Limits (%11.4g, "
      "%11.4g); Objective = %11.4g\n",
      mipsolver.submip ? "Sub-MIP" : "MIP    ", mipsolver.model_->num_col_,
      mipsolver.model_->num_row_, non_improving ? "non-" : "",
      int(numImprovingSols), new_upper_limit, upper_limit,
      mipsolver.solution_objective_);
  */
  if (mipsolver.callback_->user_callback) {
    if (mipsolver.callback_->active[kCallbackMipImprovingSolution]) {
      mipsolver.callback_->clearHighsCallbackDataOut();
      mipsolver.callback_->data_out.mip_solution = mipsolver.solution_.data();
      const bool interrupt = interruptFromCallbackWithData(
          kCallbackMipImprovingSolution, mipsolver.solution_objective_,
          "Improving solution");
      assert(!interrupt);
    }
  }

  if (mipsolver.options_mip_->mip_improving_solution_save) {
    HighsObjectiveSolution record;
    record.objective = mipsolver.solution_objective_;
    record.col_value = mipsolver.solution_;
    mipsolver.saved_objective_and_solution_.push_back(record);
  }
  FILE* file = mipsolver.improving_solution_file_;
  if (file) {
    writeLpObjective(file, *(mipsolver.orig_model_), mipsolver.solution_);
    writePrimalSolution(
        file, *(mipsolver.orig_model_), mipsolver.solution_,
        mipsolver.options_mip_->mip_improving_solution_report_sparse);
  }
}

void HighsMipSolverData::limitsToBounds(double& dual_bound,
                                        double& primal_bound,
                                        double& mip_rel_gap) const {
  const HighsLp* model = this->mipsolver.model_;
  const HighsLp* orig_model = this->mipsolver.orig_model_;

  const double offset = model->offset_;
  dual_bound = lower_bound + offset;
  if (std::abs(dual_bound) <= epsilon) dual_bound = 0;
  primal_bound = kHighsInf;
  mip_rel_gap = kHighsInf;

  if (upper_bound != kHighsInf) {
    primal_bound = upper_bound + offset;

    if (std::fabs(primal_bound) <= epsilon) primal_bound = 0;
    dual_bound = std::min(dual_bound, primal_bound);
    if (primal_bound == 0.0)
      mip_rel_gap = dual_bound == 0.0 ? 0.0 : kHighsInf;
    else
      mip_rel_gap = 100. * (primal_bound - dual_bound) / fabs(primal_bound);
  }
  primal_bound =
      std::min(mipsolver.options_mip_->objective_bound, primal_bound);

  // Adjust objective sense in case of maximization problem
  if (orig_model->sense_ == ObjSense::kMaximize) {
    dual_bound = -dual_bound;
    primal_bound = -primal_bound;
  }
}

// Interface to callbackAction, with mipsolver_objective_value since
// incumbent value (mipsolver.solution_objective_) is not right for
// callback_type = kCallbackMipSolution

bool HighsMipSolverData::interruptFromCallbackWithData(
    const int callback_type, const double mipsolver_objective_value,
    const std::string message) const {
  if (!mipsolver.callback_->callbackActive(callback_type)) return false;
  assert(!mipsolver.submip);

  double dual_bound;
  double primal_bound;
  double mip_rel_gap;
  limitsToBounds(dual_bound, primal_bound, mip_rel_gap);
  mipsolver.callback_->data_out.running_time =
      mipsolver.timer_.read(mipsolver.timer_.solve_clock);
  mipsolver.callback_->data_out.objective_function_value =
      mipsolver_objective_value;
  mipsolver.callback_->data_out.mip_node_count = mipsolver.mipdata_->num_nodes;
  mipsolver.callback_->data_out.mip_primal_bound = primal_bound;
  mipsolver.callback_->data_out.mip_dual_bound = dual_bound;
  // Option mip_rel_gap, and mip_gap in HighsInfo, are both fractions,
  // whereas mip_rel_gap in logging output (mimicked by
  // limitsToBounds) gives a percentage, so convert it a fraction
  mipsolver.callback_->data_out.mip_gap = 1e-2 * mip_rel_gap;
  return mipsolver.callback_->callbackAction(callback_type, message);
}
