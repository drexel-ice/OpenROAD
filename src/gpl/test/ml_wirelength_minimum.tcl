# Verify minimum coverage rejects an insufficient prediction set.
source helpers.tcl
source ml_wirelength_helpers.tcl
setup_ml_simple01

lassign [ml_test_files ml-minimum missing_first] prediction_file report_file
set failed [catch {
  global_placement -init_density_penalty 0.01 -skip_initial_place \
    -ml_wirelength_predictions $prediction_file \
    -ml_prediction_coverage_policy minimum \
    -ml_prediction_min_coverage 1.0 \
    -ml_wirelength_report $report_file
} message]
if { !$failed || ![string match "*GPL-1116*" $message] } {
  error "minimum coverage did not reject an insufficient prediction set: $message"
}
puts pass
