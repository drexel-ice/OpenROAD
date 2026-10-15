# Verify predictions equal to reference HPWL produce unit custom weights.
source helpers.tcl
source ml_wirelength_helpers.tcl

set reference_features [make_result_file ml-equal-reference-features.csv]
set reference_predictions [make_result_file ml-equal-reference-predictions.csv]
set reference_report [make_result_file ml-equal-reference-report.csv]
exec env \
  OR_ML_EQUAL_FEATURES=$reference_features \
  OR_ML_EQUAL_PREDICTIONS=$reference_predictions \
  OR_ML_EQUAL_REPORT=$reference_report \
  [info nameofexecutable] -no_splash -no_init -exit \
  ml_wirelength_equal_prepare.tcl

setup_ml_simple01
lassign [ml_test_files ml-equal equal] prediction_file report_file
set current_prediction_file [make_result_file ml-equal-current-predictions.csv]
write_current_hpwl_predictions \
  $prediction_file $reference_report $current_prediction_file
global_placement -init_density_penalty 0.01 -skip_initial_place \
  -ml_wirelength_predictions $current_prediction_file \
  -ml_wirelength_alpha 1 \
  -ml_wirelength_report $report_file
assert_unit_applied_weights $report_file
puts pass
