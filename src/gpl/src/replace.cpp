// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2018-2025, The OpenROAD Authors

#include "gpl/Replace.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "AbstractGraphics.h"
#include "clockBase.h"
#include "db_sta/dbNetwork.hh"
#include "db_sta/dbSta.hh"
#include "graphicsNone.h"
#include "initialPlace.h"
#include "mbff.h"
#include "mlWirelength.h"
#include "nesterovBase.h"
#include "nesterovPlace.h"
#include "odb/db.h"
#include "odb/util.h"
#include "placerBase.h"
#include "routeBase.h"
#include "rsz/Resizer.hh"
#include "sta/Graph.hh"
#include "sta/Path.hh"
#include "sta/StaMain.hh"
#include "sta/StaState.hh"
#include "timingBase.h"
#include "utl/Logger.h"
#include "utl/timer.h"
#include "utl/validation.h"

#ifdef ENABLE_GPU
#include "gpu/gpuRuntime.h"
#endif

namespace gpl {

using utl::GPL;

Replace::Replace(odb::dbDatabase* odb,
                 sta::dbSta* sta,
                 rsz::Resizer* resizer,
                 grt::GlobalRouter* router,
                 utl::Logger* logger)
    : db_(odb), sta_(sta), rs_(resizer), fr_(router), log_(logger)
{
  graphics_ = std::make_unique<GraphicsNone>();

  // Register "orig_name" report_path field: original pin name before
  // multi-bit clustering. Reads "orig_name" property off the path
  // vertex's iterm. Registered at tool init so paths can be reported
  // even when the design is loaded from an already-clustered .odb
  // without re-running cluster_flops.
  if (sta_->findReportPathField(kOrigNameProp) == nullptr) {
    sta::dbNetwork* network = sta_->getDbNetwork();
    sta_->makeReportPathField(
        kOrigNameProp,
        kOrigNameProp,
        "Orig Name",
        36,
        true,
        nullptr,
        [network](const sta::Path* path,
                  const sta::StaState* sta) -> std::string {
          if (path == nullptr) {
            return {};
          }
          const sta::Pin* pin = path->vertex(sta)->pin();
          // staToDb requires all three out-params; only iterm is used.
          odb::dbITerm* iterm = nullptr;
          odb::dbBTerm* bterm = nullptr;
          odb::dbModITerm* moditerm = nullptr;
          network->staToDb(pin, iterm, bterm, moditerm);
          if (iterm == nullptr) {
            return {};
          }
          odb::dbStringProperty* prop
              = odb::dbStringProperty::find(iterm, kOrigNameProp);
          return prop ? prop->getValue() : std::string{};
        });
  }
}

Replace::~Replace() = default;

void Replace::setGraphicsInterface(const AbstractGraphics& graphics)
{
  graphics_ = graphics.MakeNew(log_);
}

void Replace::reset()
{
  ip_.reset();
  np_.reset();

  pbc_.reset();
  nbc_.reset();

  pbVec_.clear();
  pbVec_.shrink_to_fit();
  nbVec_.clear();
  nbVec_.shrink_to_fit();

  tb_.reset();
  rb_.reset();
  cb_.reset();
}

void Replace::writeMlNetFeatures(const std::string& filename,
                                 const std::string& feature_set_id,
                                 const bool preserve_lineage)
{
  if (filename.empty() || feature_set_id.empty()) {
    log_->error(
        GPL, 1100, "ML feature filename and feature-set id are required.");
  }

  PlaceOptions options;
  auto feature_pbc = std::make_shared<PlacerBaseCommon>(db_, options, log_);
  odb::dbBlock* block = db_->getChip()->getBlock();
  std::ofstream output(filename);
  if (!output) {
    log_->error(GPL, 1101, "Cannot open ML feature file {}.", filename);
  }

  output << "# openroad_ml_features_v1\n";
  output << "# feature_set_id=" << feature_set_id << "\n";
  output << "# dbu_per_micron=" << block->getDbUnitsPerMicron() << "\n";
  output << "# net_count=" << feature_pbc->getNets().size() << "\n";
  output << "net_id,net_name,lineage_id,is_special_net,no_of_fanouts,"
            "hpwl_dbu,x_min_dbu,y_min_dbu,x_max_dbu,y_max_dbu\n";

  for (Net* net : feature_pbc->getNets()) {
    net->updateBox(false);
    odb::dbNet* db_net = net->getDbNet();
    const unsigned int net_id = db_net->getId();
    const int reference_hpwl = net->getHpwl();
    std::string lineage_id;
    auto* lineage = odb::dbStringProperty::find(db_net, "ML_ORIGIN_NET_ID");
    if (preserve_lineage && lineage != nullptr) {
      lineage_id = lineage->getValue();
    } else if (preserve_lineage) {
      // Nets created after the original export intentionally remain untagged.
      // The oracle replay reports them as unmappable instead of pretending
      // that their stage-local OpenDB id is an original net identity.
      lineage_id.clear();
    } else {
      lineage_id = feature_set_id + ":" + std::to_string(net_id);
    }
    if (!preserve_lineage && lineage == nullptr) {
      odb::dbStringProperty::create(
          db_net, "ML_ORIGIN_NET_ID", lineage_id.c_str());
    } else if (!preserve_lineage) {
      lineage->setValue(lineage_id.c_str());
    }
    auto* reference_hpwl_property
        = odb::dbIntProperty::find(db_net, "ML_REFERENCE_HPWL_DBU");
    if (reference_hpwl_property == nullptr) {
      odb::dbIntProperty::create(
          db_net, "ML_REFERENCE_HPWL_DBU", reference_hpwl);
    } else {
      reference_hpwl_property->setValue(reference_hpwl);
    }
    const std::size_t pin_count = net->getPins().size();
    const std::size_t fanout_count = pin_count == 0 ? 0 : pin_count - 1;
    output << net_id << ',' << mlCsvEscape(db_net->getName()) << ','
           << mlCsvEscape(lineage_id) << ',' << (db_net->isSpecial() ? 1 : 0)
           << ',' << fanout_count << ',' << reference_hpwl << ',' << net->lx()
           << ',' << net->ly() << ',' << net->ux() << ',' << net->uy() << '\n';
  }
  output.close();
  if (!output) {
    log_->error(
        GPL, 1102, "Failed while writing ML feature file {}.", filename);
  }
  log_->info(GPL,
             1103,
             "Exported {} ML net feature rows to {}.",
             feature_pbc->getNets().size(),
             filename);
}

void Replace::applyMlWirelengthPredictions(const PlaceOptions& options)
{
  if (options.mlWirelengthPredictions.empty()) {
    return;
  }
  if (options.mlWirelengthAlpha < 0.0f || options.mlWirelengthAlpha > 1.0f) {
    log_->error(GPL, 1104, "-ml_wirelength_alpha must be in [0, 1].");
  }
  if (!(options.mlWirelengthRatioMin > 0.0f)
      || options.mlWirelengthRatioMin > options.mlWirelengthRatioMax) {
    log_->error(
        GPL, 1105, "-ml_wirelength_ratio_limits must be positive and ordered.");
  }
  if (options.mlPredictionMinCoverage < 0.0f
      || options.mlPredictionMinCoverage > 1.0f) {
    log_->error(GPL, 1106, "-ml_prediction_min_coverage must be in [0, 1].");
  }
  const std::string& policy = options.mlPredictionCoveragePolicy;
  if (policy != "strict" && policy != "unity_fallback" && policy != "minimum") {
    log_->error(
        GPL, 1107, "Unknown ML prediction coverage policy '{}'.", policy);
  }
  const std::string& update_mode = options.mlWirelengthUpdateMode;
  if (update_mode != "fixed" && update_mode != "current_ratio"
      && update_mode != "anchored" && update_mode != "tail_unity"
      && update_mode != "tail_initial" && update_mode != "tail_current") {
    log_->error(
        GPL, 1121, "Unknown ML wirelength update mode '{}'.", update_mode);
  }
  if (options.mlWirelengthUpdateInterval <= 0) {
    log_->error(GPL, 1122, "-ml_wirelength_update_interval must be positive.");
  }
  if (!(options.mlWirelengthUpdateEma > 0.0f)
      || options.mlWirelengthUpdateEma > 1.0f) {
    log_->error(GPL, 1123, "-ml_wirelength_update_ema must be in (0, 1].");
  }
  if (options.mlWirelengthUpdateMaxDelta < 0.0f) {
    log_->error(
        GPL, 1124, "-ml_wirelength_update_max_delta must be nonnegative.");
  }
  if (options.mlWirelengthAnchoredExponent < 0.0f) {
    log_->error(
        GPL, 1125, "-ml_wirelength_anchored_exponent must be nonnegative.");
  }
  if (!(options.mlWirelengthProgressMin > 0.0f)
      || options.mlWirelengthProgressMin > options.mlWirelengthProgressMax) {
    log_->error(GPL,
                1126,
                "-ml_wirelength_progress_limits must be positive and ordered.");
  }
  const bool tail_mode = update_mode == "tail_unity"
                         || update_mode == "tail_initial"
                         || update_mode == "tail_current";
  if (tail_mode && options.mlWirelengthTailIterations <= 0) {
    log_->error(GPL,
                1133,
                "ML tail mode '{}' requires a positive "
                "-ml_wirelength_tail_iterations value.",
                update_mode);
  }

#ifdef ENABLE_GPU
  if (gpuEnabled()) {
    log_->error(GPL,
                1108,
                "ML wirelength injection is CPU-only in v1; set ENABLE_GPU=0.");
  }
#endif

  MlPredictionFile prediction_file;
  try {
    prediction_file = readMlPredictionFile(options.mlWirelengthPredictions);
  } catch (const std::exception& error) {
    log_->error(GPL, 1109, "{}", error.what());
  }

  odb::dbBlock* block = db_->getChip()->getBlock();
  if (prediction_file.dbu_per_micron != block->getDbUnitsPerMicron()) {
    log_->error(GPL,
                1110,
                "ML prediction DBU scale {} does not match design scale {}.",
                prediction_file.dbu_per_micron,
                block->getDbUnitsPerMicron());
  }
  const auto& gnets = nbc_->getGNets();
  if (prediction_file.expected_net_count != static_cast<int>(gnets.size())
      || prediction_file.records.size() != gnets.size()) {
    log_->error(
        GPL,
        1111,
        "ML prediction net count does not match GPL net count ({} metadata, "
        "{} rows, {} GPL).",
        prediction_file.expected_net_count,
        prediction_file.records.size(),
        gnets.size());
  }

  struct AppliedRecord
  {
    GNet* net;
    const MlPredictionRecord* prediction;
    int64_t hpwl;
    int64_t gpl_internal_hpwl;
    double raw_ratio = 1.0;
    double clipped_ratio = 1.0;
    double normalized_ratio = 1.0;
    double final_weight = 1.0;
    std::string reason;
  };
  std::vector<AppliedRecord> applied;
  applied.reserve(gnets.size());
  int declared_eligible_count = 0;
  int applicable_count = 0;
  int valid_count = 0;
  double clipped_sum = 0.0;

  for (GNet* gnet : gnets) {
    odb::dbNet* db_net = gnet->getPbNet()->getDbNet();
    const unsigned int net_id = db_net->getId();
    const auto found = prediction_file.records.find(net_id);
    if (found == prediction_file.records.end()) {
      log_->error(
          GPL, 1112, "ML prediction row missing for net id {}.", net_id);
    }
    const std::string expected_lineage
        = prediction_file.feature_set_id + ":" + std::to_string(net_id);
    auto* lineage = odb::dbStringProperty::find(db_net, "ML_ORIGIN_NET_ID");
    if (lineage == nullptr || lineage->getValue() != expected_lineage) {
      log_->error(
          GPL, 1113, "ML feature-set lineage mismatch for net id {}.", net_id);
    }

    auto* reference_hpwl
        = odb::dbIntProperty::find(db_net, "ML_REFERENCE_HPWL_DBU");
    if (reference_hpwl == nullptr) {
      log_->error(GPL,
                  1120,
                  "ML reference HPWL is missing for net id {}; export features "
                  "immediately after initial placement before applying "
                  "predictions.",
                  net_id);
    }

    gnet->updateBox();
    AppliedRecord row{gnet,
                      &found->second,
                      reference_hpwl->getValue(),
                      gnet->getHpwl(),
                      1.0,
                      1.0,
                      1.0,
                      1.0,
                      {}};
    if (!found->second.eligible) {
      row.reason = "ineligible";
    } else {
      declared_eligible_count++;
      if (row.hpwl <= 0) {
        row.reason = "zero_hpwl";
      } else if (!found->second.predicted_wirelength_dbu.has_value()) {
        applicable_count++;
        row.reason = "missing_or_invalid_prediction";
      } else {
        applicable_count++;
        valid_count++;
        row.raw_ratio = *found->second.predicted_wirelength_dbu / row.hpwl;
        row.clipped_ratio
            = std::clamp(row.raw_ratio,
                         static_cast<double>(options.mlWirelengthRatioMin),
                         static_cast<double>(options.mlWirelengthRatioMax));
        clipped_sum += row.clipped_ratio;
        row.reason = "applied";
      }
    }
    applied.push_back(std::move(row));
  }

  if (declared_eligible_count != prediction_file.eligible_net_count) {
    log_->error(
        GPL,
        1114,
        "ML eligible net count does not match metadata ({} rows, {} metadata).",
        declared_eligible_count,
        prediction_file.eligible_net_count);
  }
  const double coverage
      = applicable_count == 0
            ? 1.0
            : static_cast<double>(valid_count) / applicable_count;
  if (policy == "strict" && valid_count != applicable_count) {
    log_->error(GPL,
                1115,
                "Strict ML coverage failed: {}/{} eligible nets have valid "
                "predictions.",
                valid_count,
                applicable_count);
  }
  if (policy == "minimum" && coverage < options.mlPredictionMinCoverage) {
    log_->error(GPL,
                1116,
                "ML prediction coverage {:.3f} is below required {:.3f}.",
                coverage,
                options.mlPredictionMinCoverage);
  }

  const double clipped_mean
      = valid_count == 0 ? 1.0 : clipped_sum / valid_count;
  for (AppliedRecord& row : applied) {
    if (row.reason == "applied") {
      row.normalized_ratio = row.clipped_ratio / clipped_mean;
      row.final_weight
          = options.mlWirelengthAlpha == 0.0f || tail_mode
                ? 1.0
                : (1.0 - options.mlWirelengthAlpha)
                      + options.mlWirelengthAlpha * row.normalized_ratio;
      row.net->setMlWirelengthState(*row.prediction->predicted_wirelength_dbu,
                                    row.hpwl,
                                    row.normalized_ratio);
    }
    row.net->setCustomWeight(static_cast<float>(row.final_weight));
  }

  if (!options.mlWirelengthReport.empty()) {
    std::ofstream report(options.mlWirelengthReport);
    if (!report) {
      log_->error(GPL,
                  1117,
                  "Cannot open ML wirelength report {}.",
                  options.mlWirelengthReport);
    }
    report << "net_id,net_name,lineage_id,eligible,predicted_wirelength_dbu,"
              "current_hpwl_dbu,raw_ratio,clipped_ratio,normalized_ratio,"
              "custom_weight,reason,gpl_internal_hpwl_dbu\n";
    for (const AppliedRecord& row : applied) {
      odb::dbNet* db_net = row.net->getPbNet()->getDbNet();
      auto* lineage = odb::dbStringProperty::find(db_net, "ML_ORIGIN_NET_ID");
      report << db_net->getId() << ',' << mlCsvEscape(db_net->getName()) << ','
             << mlCsvEscape(lineage ? lineage->getValue() : "") << ','
             << (row.prediction->eligible ? 1 : 0) << ',';
      if (row.prediction->predicted_wirelength_dbu.has_value()) {
        report << *row.prediction->predicted_wirelength_dbu;
      }
      report << ',' << row.hpwl << ',' << row.raw_ratio << ','
             << row.clipped_ratio << ',' << row.normalized_ratio << ','
             << row.final_weight << ',' << row.reason << ','
             << row.gpl_internal_hpwl << '\n';
    }
    report.close();
    if (!report) {
      log_->error(GPL,
                  1118,
                  "Failed while writing ML wirelength report {}.",
                  options.mlWirelengthReport);
    }
  }

  if ((update_mode == "current_ratio" || update_mode == "anchored")
      && !options.mlWirelengthIterationReport.empty()) {
    std::ofstream report(options.mlWirelengthIterationReport);
    if (!report) {
      log_->error(GPL,
                  1131,
                  "Cannot open ML weight iteration report {}.",
                  options.mlWirelengthIterationReport);
    }
    report << "iteration,mode,eligible_nets,current_hpwl_sum_dbu,"
              "raw_signal_mean,bounded_signal_mean,normalized_signal_mean,"
              "target_weight_mean,applied_weight_mean,applied_weight_min,"
              "applied_weight_max,changed_nets\n";
    if (!report) {
      log_->error(GPL,
                  1132,
                  "Failed while writing ML weight iteration report {}.",
                  options.mlWirelengthIterationReport);
    }
  }

  log_->info(GPL,
             1119,
             "Applied ML wirelength weights to {}/{} eligible nets "
             "(coverage {:.3f}, alpha {:.3f}).",
             valid_count,
             applicable_count,
             coverage,
             options.mlWirelengthAlpha);
  log_->info(GPL,
             1127,
             "ML wirelength update mode is '{}' (interval {}, EMA {:.3f}, "
             "maximum fractional change {:.3f}).",
             update_mode,
             options.mlWirelengthUpdateInterval,
             options.mlWirelengthUpdateEma,
             options.mlWirelengthUpdateMaxDelta);
  log_->metric("globalplace__ml__declared_eligible_nets",
               declared_eligible_count);
  log_->metric("globalplace__ml__applicable_nets", applicable_count);
  log_->metric("globalplace__ml__valid_predictions", valid_count);
  log_->metric("globalplace__ml__prediction_coverage", coverage);
  log_->metric("globalplace__ml__alpha", options.mlWirelengthAlpha);
}

void Replace::addPlacementCluster(const Cluster& cluster)
{
  clusters_.emplace_back(cluster);
}

void Replace::checkHasCoreRows()
{
  if (!dbHasCoreRows(db_)) {
    log_->error(
        GPL,
        130,
        "No rows defined in design. Use initialize_floorplan to add rows.");
  }
}

// Runs before PlacerBaseCommon is built: -place_ios makes an unplaced port a
// normal state there, so an unsupported design has to be rejected first.
void Replace::checkPlaceIosSupported(const PlaceOptions& options)
{
  if (!options.placeIosMode) {
    return;
  }
  if (db_->getChip()->getBlock()->getDieAreaPolygon().getPoints().size() > 5) {
    log_->error(GPL,
                173,
                "Concurrent IO placement does not support non-rectangular die. "
                "Please drop -place_ios or use a rectangular die.");
  }
}

void Replace::doIncrementalPlace(const int threads, const PlaceOptions& options)
{
  checkHasCoreRows();
  log_->info(GPL, 83, "Execute incremental mode global placement.");
  int placed_cnt = 0;
  int unplaced_cnt = 0;
  bool is_pbc_new = (pbc_ == nullptr);

  if (is_pbc_new) {
    pbc_ = std::make_shared<PlacerBaseCommon>(db_, options, log_);
  }

  // Count placed vs unplaced, and conditionally lock them down
  auto block = db_->getChip()->getBlock();
  for (auto inst : block->getInsts()) {
    auto status = inst->getPlacementStatus();
    if (status == odb::dbPlacementStatus::PLACED) {
      if (is_pbc_new) {
        pbc_->dbToPb(inst)->lock();
      }
      ++placed_cnt;
    } else if (!status.isPlaced()) {
      ++unplaced_cnt;
    }
  }

  if (is_pbc_new) {
    pbVec_.push_back(std::make_shared<PlacerBase>(db_, pbc_, log_, true));

    for (auto pd : db_->getChip()->getBlock()->getRegions()) {
      for (auto group : pd->getGroups()) {
        pbVec_.push_back(
            std::make_shared<PlacerBase>(db_, pbc_, log_, true, group));
      }
    }

    total_placeable_insts_ = 0;
    for (const auto& pb : pbVec_) {
      total_placeable_insts_ += pb->placeInsts().size();
    }
  }

  log_->info(GPL, 154, "Identified {} placed instances", placed_cnt);
  log_->info(GPL, 155, "Identified {} not placed instances", unplaced_cnt);

  if (unplaced_cnt == 0) {
    // Everything was already placed so we do the old incremental mode
    // which just skips initial placement and runs nesterov.
    log_->info(GPL,
               156,
               "Identified all instances as placed. Skipping incremental mode");
    return;
  }

  // Roughly place the unplaced objects (allow more overflow).
  // Limit iterations to prevent objects drifting too far or
  // non-convergence.
  PlaceOptions locked_options = options;
  locked_options.overflow = std::max(options.overflow, 0.2f);
  locked_options.nesterovPlaceMaxIter = 300;

  // Use uniform density for incremental runs to fill gaps effectively
  if (!options.uniformTargetDensityMode) {
    locked_options.uniformTargetDensityMode = true;
  }

  doInitialPlace(threads, locked_options);
  const int iter = doNesterovPlace(threads, locked_options);

  // Finish the overflow resolution from the locked placement
  log_->info(GPL, 133, "Unlocking all instances");
  for (auto& pb : pbVec_) {
    pb->unlockAll();
  }

  if (options.overflow < locked_options.overflow) {
    PlaceOptions final_options = options;
    final_options.uniformTargetDensityMode = true;
    final_options.initDensityPenaltyFactor = 1;

    doNesterovPlace(threads, final_options, iter + 1);
  }
}

void Replace::doPlace(const int threads, const PlaceOptions& options)
{
  utl::Timer timer;
  doInitialPlace(threads, options);
  doNesterovPlace(threads, options);
  log_->info(GPL, 500, "Runtime: {:.2f}s", timer.elapsed());
}

void Replace::doInitialPlace(const int threads, const PlaceOptions& options)
{
  checkHasCoreRows();
  checkPlaceIosSupported(options);
  if (pbc_ == nullptr) {
    pbc_ = std::make_shared<PlacerBaseCommon>(db_, options, log_);

    pbVec_.push_back(std::make_shared<PlacerBase>(db_, pbc_, log_, true));

    for (auto pd : db_->getChip()->getBlock()->getRegions()) {
      for (auto group : pd->getGroups()) {
        pbVec_.push_back(
            std::make_shared<PlacerBase>(db_, pbc_, log_, true, group));
      }
    }

    if (pbVec_.front()->placeInsts().empty()) {
      log_->warn(
          GPL,
          123,
          "No placeable instances in the top-level region. Removing it.");
      pbVec_.erase(pbVec_.begin());
    }

    total_placeable_insts_ = 0;
    for (const auto& pb : pbVec_) {
      total_placeable_insts_ += pb->placeInsts().size();
    }
  }

  const InitialPlaceVars ipVars(options, gui_debug_initial_);

  std::unique_ptr<InitialPlace> ip(
      new InitialPlace(ipVars, pbc_, pbVec_, graphics_->MakeNew(log_), log_));
  ip_ = std::move(ip);
  ip_->doBicgstabPlace(threads);
}

void Replace::runMBFF(const int max_sz,
                      const float alpha,
                      const float beta,
                      const int threads,
                      const int num_paths,
                      const float clock_power_weight)
{
  MBFF pntset(db_,
              sta_,
              log_,
              rs_,
              threads,
              20,
              num_paths,
              gui_debug_,
              graphics_->MakeNew(log_));
  pntset.Run(max_sz, alpha, beta, clock_power_weight);
}

bool Replace::initNesterovPlace(const PlaceOptions& options,
                                const int threads,
                                bool check_density)
{
  if (!pbc_) {
    pbc_ = std::make_shared<PlacerBaseCommon>(db_, options, log_);

    pbVec_.push_back(
        std::make_shared<PlacerBase>(db_, pbc_, log_, check_density));

    for (auto pd : db_->getChip()->getBlock()->getRegions()) {
      for (auto group : pd->getGroups()) {
        pbVec_.push_back(std::make_shared<PlacerBase>(
            db_, pbc_, log_, check_density, group));
      }
    }

    total_placeable_insts_ = 0;
    for (const auto& pb : pbVec_) {
      total_placeable_insts_ += pb->placeInsts().size();
    }
  }

  if (total_placeable_insts_ == 0) {
    log_->warn(GPL, 136, "No placeable instances - skipping placement.");
    return false;
  }

  if (!nbc_) {
    const NesterovBaseVars nbVars(options);

    nbc_ = std::make_shared<NesterovBaseCommon>(
        nbVars, pbc_, log_, threads, clusters_);

    applyMlWirelengthPredictions(options);

    for (const auto& pb : pbVec_) {
      nbVec_.push_back(std::make_shared<NesterovBase>(nbVars, pb, nbc_, log_));
    }
  }

  if (!rb_) {
    const RouteBaseVars rbVars(options);
    rb_ = std::make_shared<RouteBase>(rbVars, db_, fr_, nbc_, nbVec_, log_);
  }

  if (!tb_) {
    tb_ = std::make_shared<TimingBase>(nbc_, fr_, rs_, log_);
    tb_->setTimingNetWeightOverflows(options.timingNetWeightOverflows);
    tb_->setTimingNetWeightMax(options.timingNetWeightMax);
    tb_->setTimingNetsPercentage(options.timingDrivenNetsPercentage);
    tb_->setRepairTiming(options.timingDrivenRepairTiming);
    tb_->setRepairTnsEndPercent(options.timingDrivenRepairTnsEndPercent);
  }

  if (!cb_ && options.virtualCtsMode) {
    float skew_fraction = options.virtualCtsMaxSkewFraction;
    // Clamp to a sane range; a negative value yields negative insertion
    // delays and values above the clock period make no physical sense.
    if (skew_fraction < 0.0f || skew_fraction > 1.0f) {
      log_->warn(GPL,
                 165,
                 "virtual_cts_max_skew_fraction {} out of range [0, 1]; "
                 "clamping.",
                 skew_fraction);
      skew_fraction = std::clamp(skew_fraction, 0.0f, 1.0f);
    }
    cb_ = std::make_shared<ClockBase>(sta_, db_, log_);
    cb_->setMaxSkewFraction(skew_fraction);
  }

  if (!np_) {
    NesterovPlaceVars npVars(options);

    npVars.debug = gui_debug_;
    npVars.debug_pause_iterations = gui_debug_pause_iterations_;
    npVars.debug_update_iterations = gui_debug_update_iterations_;
    npVars.debug_draw_bins = gui_debug_draw_bins_;
    npVars.debug_inst = gui_debug_inst_;
    npVars.debug_start_iter = gui_debug_start_iter_;
    npVars.debug_rudy_start = gui_debug_rudy_start_;
    npVars.debug_rudy_stride = gui_debug_rudy_stride_;
    npVars.debug_generate_images = gui_debug_generate_images_;
    npVars.debug_images_path = gui_debug_images_path_;

    for (const auto& nb : nbVec_) {
      nb->setNpVars(&npVars);
    }

    np_ = std::make_unique<NesterovPlace>(npVars,
                                          pbc_,
                                          nbc_,
                                          pbVec_,
                                          nbVec_,
                                          rb_,
                                          tb_,
                                          cb_,
                                          graphics_->MakeNew(log_),
                                          log_);
  }
  // Ensure these get set even if np_ already exists.
  np_->setTargetOverflow(options.overflow);
  np_->setMaxIters(options.nesterovPlaceMaxIter);
  return true;
}

int Replace::doNesterovPlace(const int threads,
                             const PlaceOptions& options,
                             const int start_iter)
{
  checkHasCoreRows();
  checkPlaceIosSupported(options);

  if (options.placeIosMode) {
    log_->info(GPL, 168, "Concurrent IO placement enabled.");
  }

  if (!initNesterovPlace(options, threads, true)) {
    return 0;
  }

  log_->info(GPL, 84, "---- Execute Nesterov Global Placement.");
  if (options.timingDrivenMode) {
    rs_->resizeSlackPreamble();
  }

  auto start = std::chrono::high_resolution_clock::now();

  int return_do_nesterov = np_->doNesterovPlace(start_iter);

  reportHpwlMetric();

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed = end - start;
  debugPrint(log_,
             GPL,
             "runtime",
             1,
             "NP->doNesterovPlace() runtime: {} seconds ",
             elapsed.count());

  if (options.enable_routing_congestion) {
    fr_->setAllowCongestion(true);
    fr_->setCongestionIterations(0);
    fr_->setCriticalNetsPercentage(0);
    fr_->globalRoute();
  }
  return return_do_nesterov;
}

// Same evaluator dpl reports route__wirelength__estimated with, so the
// global and detailed placement numbers are directly comparable.
void Replace::reportHpwlMetric()
{
  odb::dbBlock* block = db_->getChip()->getBlock();
  const int64_t hpwl = odb::WireLengthEvaluator(block).hpwl();
  log_->info(GPL, 1018, "Final HPWL (um): {:.2f}", block->dbuToMicrons(hpwl));
  log_->metric("route__wirelength__estimated", block->dbuToMicrons(hpwl));
}

float Replace::getUniformTargetDensity(const PlaceOptions& options,
                                       const int threads)
{
  log_->info(GPL, 22, "Initialize gpl and calculate uniform density.");
  log_->redirectStringBegin();

  PlaceOptions options_no_io = options;
  options_no_io.skipIo();  // in case bterms are not placed

  float density = 1.0f;
  if (initNesterovPlace(options_no_io, threads, false)) {
    density = nbVec_[0]->getUniformTargetDensity();
  }

  log_->redirectStringEnd();  // discard output
  return density;
}

void Replace::setDebug(const int pause_iterations,
                       const int update_iterations,
                       const bool draw_bins,
                       const bool initial,
                       odb::dbInst* inst,
                       const int start_iter,
                       const int start_rudy,
                       const int rudy_stride,
                       const bool generate_images,
                       const std::string& images_path)
{
  gui_debug_ = true;
  gui_debug_pause_iterations_ = pause_iterations;
  gui_debug_update_iterations_ = update_iterations;
  gui_debug_draw_bins_ = draw_bins;
  gui_debug_initial_ = initial;
  gui_debug_inst_ = inst;
  gui_debug_start_iter_ = start_iter;
  gui_debug_rudy_start_ = start_rudy;
  gui_debug_rudy_stride_ = rudy_stride;
  gui_debug_generate_images_ = generate_images;
  gui_debug_images_path_ = images_path;
}

void PlaceOptions::validate(utl::Logger* logger)
{
  utl::Validator val(logger, GPL);

  val.check_positive("bin_grid_count", binGridCntX, 400);
  val.check_range("density", density, 0.0f, 1.0f, 401);
  val.check_positive("init_density_penalty", initDensityPenaltyFactor, 402);
  val.check_positive("init_wirelength_coef", initWireLengthCoef, 403);
  val.check_positive("min_phi_coef", minPhiCoef, 404);
  val.check_positive("max_phi_coef", maxPhiCoef, 405);
  val.check_range("overflow", overflow, 0.0f, 1.0f, 406);
  val.check_non_negative("pad_left", padLeft, 407);
  val.check_non_negative("pad_right", padRight, 408);
  val.check_positive("reference_hpwl", referenceHpwl, 409);

  val.check_non_negative("initial_place_max_iter", initialPlaceMaxIter, 410);
  val.check_positive("initial_place_max_fanout", initialPlaceMaxFanout, 411);

  val.check_positive(
      "routability_target_rc_metric", routabilityTargetRcMetric, 412);
  val.check_range("routability_snapshot_overflow",
                  routabilitySnapshotOverflow,
                  0.0f,
                  1.0f,
                  413);
  val.check_range(
      "routability_check_overflow", routabilityCheckOverflow, 0.0f, 1.0f, 414);
  val.check_range(
      "routability_max_density", routabilityMaxDensity, 0.0f, 1.0f, 415);
  val.check_positive(
      "routability_inflation_ratio_coef", routabilityInflationRatioCoef, 416);
  val.check_positive(
      "routability_max_inflation_ratio", routabilityMaxInflationRatio, 417);
  val.check_non_negative(
      "routability_rc_coefficients k1", routabilityRcK1, 418);
  val.check_non_negative(
      "routability_rc_coefficients k2", routabilityRcK2, 419);
  val.check_non_negative(
      "routability_rc_coefficients k3", routabilityRcK3, 420);
  val.check_non_negative(
      "routability_rc_coefficients k4", routabilityRcK4, 421);

  for (const auto& timingNetOverflow : timingNetWeightOverflows) {
    val.check_positive(
        "timing_driven_net_reweight_overflow", timingNetOverflow, 422);
  }
  val.check_positive("timing_driven_net_weight_max", timingNetWeightMax, 423);
  val.check_range("timing_driven_nets_percentage",
                  timingDrivenNetsPercentage,
                  0.0f,
                  100.0f,
                  424);
  val.check_range(
      "keep_resize_below_overflow", keepResizeBelowOverflow, 0.0f, 1.0f, 425);
}

void PlaceOptions::skipIo()
{
  skipIoMode = true;
  initialPlaceMaxIter = 0;
  timingDrivenMode = false;
  routabilityDrivenMode = false;
}

}  // namespace gpl
