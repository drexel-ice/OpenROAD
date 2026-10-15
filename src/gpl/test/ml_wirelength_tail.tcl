# Verify late-tail activation produces nonuniform weights after convergence.
source helpers.tcl
source ml_wirelength_helpers.tcl
setup_ml_simple01

lassign [ml_test_files ml-tail nonuniform] prediction_file report_file
set tail_report [make_result_file ml-tail-weights.csv]
global_placement -init_density_penalty 0.01 -skip_initial_place \
  -ml_wirelength_predictions $prediction_file \
  -ml_wirelength_alpha 0.25 \
  -ml_wirelength_update_mode tail_current \
  -ml_wirelength_tail_iterations 10 \
  -ml_wirelength_tail_report $tail_report \
  -ml_wirelength_report $report_file

# Tail modes must leave every initial custom weight at unity.
assert_unit_applied_weights $report_file

set below_one 0
set above_one 0
set rows 0
set report [open $tail_report r]
gets $report header
while { [gets $report row] >= 0 } {
  set fields [split $row ,]
  if { [lindex $fields 8] ne "tail_current" } {
    error "unexpected tail mode [lindex $fields 8]"
  }
  set weight [lindex $fields 7]
  set below_one [expr {$below_one || $weight < 0.99}]
  set above_one [expr {$above_one || $weight > 1.01}]
  incr rows
}
close $report
if { $rows == 0 || !$below_one || !$above_one } {
  error "tail activation did not produce nonuniform weights around one"
}
puts pass
