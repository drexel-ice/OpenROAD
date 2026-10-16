// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace utl {

// Versioned, injection-point-independent representation of the external
// per-net wirelength prediction contract.
struct MlPredictionRecord
{
  bool eligible = false;
  std::optional<double> predicted_wirelength_dbu;
};

struct MlPredictionFile
{
  std::string feature_set_id;
  int dbu_per_micron = 0;
  int expected_net_count = -1;
  int eligible_net_count = -1;
  std::unordered_map<unsigned int, MlPredictionRecord> records;
};

MlPredictionFile readMlPredictionFile(const std::string& filename);
std::string mlCsvEscape(const std::string& value);

}  // namespace utl
