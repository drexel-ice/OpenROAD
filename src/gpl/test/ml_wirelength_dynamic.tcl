# Verify dynamic ML weighting refreshes nonuniform weights during placement.
source helpers.tcl
source ml_wirelength_helpers.tcl
setup_ml_simple01

lassign [ml_test_files ml-dynamic nonuniform] prediction_file report_file
set iteration_report [make_result_file ml-dynamic-iterations.csv]
global_placement -init_density_penalty 0.01 -skip_initial_place \
  -ml_wirelength_predictions $prediction_file \
  -ml_wirelength_alpha 0.25 \
  -ml_wirelength_update_mode current_ratio \
  -ml_wirelength_update_interval 1 \
  -ml_wirelength_update_ema 0.2 \
  -ml_wirelength_update_max_delta 0.1 \
  -ml_wirelength_iteration_report $iteration_report \
  -ml_wirelength_report $report_file

set input [open $iteration_report r]
gets $input header
set refresh_count 0
set changed_count 0
while { [gets $input row] >= 0 } {
  set fields [split $row ,]
  if { [lindex $fields 1] ne "current_ratio" } {
    error "unexpected dynamic update mode [lindex $fields 1]"
  }
  incr refresh_count
  set changed_count [expr {$changed_count + [lindex $fields 11]}]
}
close $input
if { $refresh_count == 0 || $changed_count == 0 } {
  error "adaptive ML weights were not refreshed"
}
puts pass
