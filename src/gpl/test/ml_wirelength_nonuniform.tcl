# Verify nonuniform predictions change both weights and placement.
source helpers.tcl
source ml_wirelength_helpers.tcl
setup_ml_simple01

lassign [ml_test_files ml-nonuniform nonuniform] prediction_file report_file
global_placement -init_density_penalty 0.01 -skip_initial_place \
  -ml_wirelength_predictions $prediction_file \
  -ml_wirelength_alpha 1 \
  -ml_wirelength_report $report_file

set below_one 0
set above_one 0
set report [open $report_file r]
gets $report header
while { [gets $report row] >= 0 } {
  set fields [split $row ,]
  if { [lindex $fields 10] eq "applied" } {
    set weight [lindex $fields 9]
    set below_one [expr {$below_one || $weight < 0.99}]
    set above_one [expr {$above_one || $weight > 1.01}]
  }
}
close $report
if { !$below_one || !$above_one } {
  error "nonuniform predictions did not produce weights on both sides of one"
}

set def_file [make_result_file ml_wirelength_nonuniform.def]
write_def $def_file
if { ![diff_files $def_file simple01.defok] } {
  error "nonuniform ML weights did not change placement"
}
puts pass
