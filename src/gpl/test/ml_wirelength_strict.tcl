# Verify strict coverage rejects any missing eligible prediction.
source helpers.tcl
source ml_wirelength_helpers.tcl
setup_ml_simple01

lassign [ml_test_files ml-strict missing_first] prediction_file report_file
set failed [catch {
  global_placement -init_density_penalty 0.01 -skip_initial_place \
    -ml_wirelength_predictions $prediction_file \
    -ml_prediction_coverage_policy strict \
    -ml_wirelength_report $report_file
} message]
if { !$failed || ![string match "*GPL-1115*" $message] } {
  error "strict coverage did not reject a missing prediction: $message"
}
puts pass
