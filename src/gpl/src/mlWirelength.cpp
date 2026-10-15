// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include "mlWirelength.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace gpl {

namespace {

std::string trim(std::string value)
{
  const auto first
      = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
          return std::isspace(c);
        });
  const auto last
      = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
          return std::isspace(c);
        }).base();
  if (first >= last) {
    return {};
  }
  return std::string(first, last);
}

std::vector<std::string> splitCsv(const std::string& line)
{
  // Prediction files intentionally contain only numeric fields, so quoted
  // CSV parsing is unnecessary here.  Audit names live in the feature file.
  std::vector<std::string> fields;
  std::stringstream stream(line);
  std::string field;
  while (std::getline(stream, field, ',')) {
    fields.push_back(trim(field));
  }
  if (!line.empty() && line.back() == ',') {
    fields.emplace_back();
  }
  return fields;
}

template <typename Value, typename Parser>
Value parseExactNumber(const std::string& text,
                       const std::string& description,
                       const int line_number,
                       Parser parser)
{
  std::size_t consumed = 0;
  try {
    const Value value = parser(text, &consumed);
    if (consumed != text.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return value;
  } catch (const std::exception&) {
    throw std::runtime_error("invalid " + description + " at line "
                             + std::to_string(line_number));
  }
}

void parseMetadata(const std::string& line,
                   const int line_number,
                   MlPredictionFile& result,
                   std::unordered_set<std::string>& seen_metadata)
{
  const std::string body = trim(line.substr(1));
  const auto equals = body.find('=');
  if (equals == std::string::npos) {
    return;
  }
  const std::string key = trim(body.substr(0, equals));
  const std::string value = trim(body.substr(equals + 1));
  if (key != "feature_set_id" && key != "dbu_per_micron"
      && key != "expected_net_count" && key != "eligible_net_count") {
    return;
  }
  if (!seen_metadata.insert(key).second) {
    throw std::runtime_error("duplicate ML prediction metadata '" + key
                             + "' at line " + std::to_string(line_number));
  }
  if (key == "feature_set_id") {
    result.feature_set_id = value;
  } else if (key == "dbu_per_micron") {
    result.dbu_per_micron = parseExactNumber<int>(
        value,
        "ML prediction dbu_per_micron",
        line_number,
        [](const std::string& item, std::size_t* consumed) {
          return std::stoi(item, consumed);
        });
  } else if (key == "expected_net_count") {
    result.expected_net_count = parseExactNumber<int>(
        value,
        "ML prediction expected_net_count",
        line_number,
        [](const std::string& item, std::size_t* consumed) {
          return std::stoi(item, consumed);
        });
  } else if (key == "eligible_net_count") {
    result.eligible_net_count = parseExactNumber<int>(
        value,
        "ML prediction eligible_net_count",
        line_number,
        [](const std::string& item, std::size_t* consumed) {
          return std::stoi(item, consumed);
        });
  }
}

}  // namespace

MlPredictionFile readMlPredictionFile(const std::string& filename)
{
  std::ifstream input(filename);
  if (!input) {
    throw std::runtime_error("cannot open ML prediction file: " + filename);
  }

  MlPredictionFile result;
  bool saw_magic = false;
  bool saw_header = false;
  std::unordered_set<std::string> seen_metadata;
  std::string line;
  int line_number = 0;
  while (std::getline(input, line)) {
    line_number++;
    if (line.empty()) {
      continue;
    }
    if (line[0] == '#') {
      if (trim(line) == "# openroad_ml_predictions_v1") {
        if (saw_magic) {
          throw std::runtime_error("duplicate ML prediction schema marker");
        }
        saw_magic = true;
      } else {
        parseMetadata(line, line_number, result, seen_metadata);
      }
      continue;
    }
    if (!saw_header) {
      if (trim(line) != "net_id,eligible,predicted_wirelength_dbu") {
        throw std::runtime_error("invalid ML prediction header at line "
                                 + std::to_string(line_number));
      }
      saw_header = true;
      continue;
    }

    const auto fields = splitCsv(line);
    if (fields.size() != 3) {
      throw std::runtime_error("invalid ML prediction row at line "
                               + std::to_string(line_number));
    }
    const auto net_id_value = parseExactNumber<unsigned long>(
        fields[0],
        "ML prediction net id",
        line_number,
        [](const std::string& value, std::size_t* consumed) {
          return std::stoul(value, consumed);
        });
    if (net_id_value > std::numeric_limits<unsigned int>::max()) {
      throw std::runtime_error("ML prediction net id out of range at line "
                               + std::to_string(line_number));
    }
    const unsigned int net_id = static_cast<unsigned int>(net_id_value);
    if (fields[1] != "0" && fields[1] != "1") {
      throw std::runtime_error(
          "ML prediction eligibility must be 0 or 1 at line "
          + std::to_string(line_number));
    }
    MlPredictionRecord record;
    record.eligible = fields[1] == "1";
    if (!fields[2].empty()) {
      const double prediction = parseExactNumber<double>(
          fields[2],
          "ML predicted wirelength",
          line_number,
          [](const std::string& value, std::size_t* consumed) {
            return std::stod(value, consumed);
          });
      // A linear regressor trained on zero-valued routed-length targets can
      // emit finite nonpositive estimates.  Preserve the supplied model
      // output; the positive ratio limits provide the safety boundary.
      if (std::isfinite(prediction)) {
        record.predicted_wirelength_dbu = prediction;
      }
    }
    if (!result.records.emplace(net_id, record).second) {
      throw std::runtime_error("duplicate ML prediction net id "
                               + std::to_string(net_id));
    }
  }

  if (!saw_magic || !saw_header || result.feature_set_id.empty()
      || result.dbu_per_micron <= 0 || result.expected_net_count < 0
      || result.eligible_net_count < 0) {
    throw std::runtime_error("incomplete ML prediction file metadata");
  }
  return result;
}

std::string mlCsvEscape(const std::string& value)
{
  if (value.find_first_of(",\"\r\n") == std::string::npos) {
    return value;
  }
  std::string escaped = "\"";
  for (const char c : value) {
    if (c == '\"') {
      escaped += "\"\"";
    } else {
      escaped += c;
    }
  }
  escaped += "\"";
  return escaped;
}

}  // namespace gpl
