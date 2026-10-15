# Verify unity fallback assigns unit weight to a missing prediction.
source helpers.tcl
source ml_wirelength_helpers.tcl
setup_ml_simple01

lassign [ml_test_files ml-fallback missing_first] prediction_file report_file
global_placement -init_density_penalty 0.01 -skip_initial_place \
  -ml_wirelength_predictions $prediction_file \
  -ml_prediction_coverage_policy unity_fallback \
  -ml_wirelength_alpha 1 \
  -ml_wirelength_report $report_file

set found_fallback 0
set report [open $report_file r]
gets $report header
while { [gets $report row] >= 0 } {
  set fields [split $row ,]
  if { [lindex $fields 10] eq "missing_or_invalid_prediction" } {
    set found_fallback 1
    if { abs([lindex $fields 9] - 1.0) > 1e-6 } {
      error "fallback prediction did not receive unit weight"
    }
  }
}
close $report
if { !$found_fallback } {
  error "unity_fallback report contains no fallback row"
}
puts pass
