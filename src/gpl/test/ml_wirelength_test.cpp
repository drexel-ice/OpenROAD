// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, The OpenROAD Authors

#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"
#include "mlWirelength.h"

namespace gpl {
namespace {

class PredictionFile
{
 public:
  explicit PredictionFile(const std::string& contents)
  {
    static int sequence = 0;
    path_
        = std::filesystem::temp_directory_path()
          / ("openroad-ml-predictions-" + std::to_string(sequence++) + ".csv");
    std::ofstream output(path_);
    output << contents;
  }

  ~PredictionFile() { std::filesystem::remove(path_); }

  std::string string() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

const char* kMetadata = R"(# openroad_ml_predictions_v1
# feature_set_id=campaign:design
# dbu_per_micron=2000
# expected_net_count=2
# eligible_net_count=1
net_id,eligible,predicted_wirelength_dbu
)";

TEST(MlWirelength, ReadsValidPredictionContract)
{
  PredictionFile file(std::string(kMetadata) + "11,1,123.5\n12,0,\n");
  const MlPredictionFile predictions = readMlPredictionFile(file.string());

  EXPECT_EQ(predictions.feature_set_id, "campaign:design");
  EXPECT_EQ(predictions.dbu_per_micron, 2000);
  ASSERT_EQ(predictions.records.size(), 2);
  ASSERT_TRUE(predictions.records.at(11).predicted_wirelength_dbu.has_value());
  EXPECT_DOUBLE_EQ(*predictions.records.at(11).predicted_wirelength_dbu, 123.5);
  EXPECT_FALSE(predictions.records.at(12).predicted_wirelength_dbu.has_value());
}

TEST(MlWirelength, RejectsDuplicateNetIds)
{
  PredictionFile file(std::string(kMetadata) + "11,1,123\n11,0,\n");
  EXPECT_THROW(readMlPredictionFile(file.string()), std::runtime_error);
}

TEST(MlWirelength, RejectsPartiallyParsedNumbers)
{
  PredictionFile bad_id(std::string(kMetadata) + "11x,1,123\n12,0,\n");
  EXPECT_THROW(readMlPredictionFile(bad_id.string()), std::runtime_error);

  const std::string bad_metadata
      = "# openroad_ml_predictions_v1\n"
        "# feature_set_id=campaign:design\n"
        "# dbu_per_micron=2000dbu\n"
        "# expected_net_count=1\n"
        "# eligible_net_count=1\n"
        "net_id,eligible,predicted_wirelength_dbu\n11,1,123\n";
  PredictionFile bad_dbu(bad_metadata);
  EXPECT_THROW(readMlPredictionFile(bad_dbu.string()), std::runtime_error);
}

TEST(MlWirelength, RejectsDuplicateMetadata)
{
  const std::string duplicate
      = "# openroad_ml_predictions_v1\n"
        "# feature_set_id=first\n"
        "# feature_set_id=second\n"
        "# dbu_per_micron=2000\n"
        "# expected_net_count=1\n"
        "# eligible_net_count=1\n"
        "net_id,eligible,predicted_wirelength_dbu\n11,1,123\n";
  PredictionFile file(duplicate);
  EXPECT_THROW(readMlPredictionFile(file.string()), std::runtime_error);
}

TEST(MlWirelength, TreatsNonpositivePredictionsAsMissing)
{
  PredictionFile file(std::string(kMetadata) + "11,1,0\n12,1,-4.5\n");
  const MlPredictionFile predictions = readMlPredictionFile(file.string());

  EXPECT_FALSE(predictions.records.at(11).predicted_wirelength_dbu.has_value());
  EXPECT_FALSE(predictions.records.at(12).predicted_wirelength_dbu.has_value());
}

TEST(MlWirelength, EscapesAuditNamesAsCsv)
{
  EXPECT_EQ(mlCsvEscape("plain"), "plain");
  EXPECT_EQ(mlCsvEscape("net,\"quoted\""), "\"net,\"\"quoted\"\"\"");
}

}  // namespace
}  // namespace gpl
