# Verify predicted wirelength scales placement-estimated wire R and C through
# the EST adapter without using GPL custom weights.
source "helpers.tcl"
read_lef ../../gpl/test/nangate45.lef
read_liberty Nangate45/Nangate45_typ.lib
read_def ../../gpl/test/simple01.def

set_wire_rc -resistance 0.1 -capacitance 0.1
# The ORFS injection point reads an already globally placed ODB.  Reproduce
# that precondition here without running the Nesterov loop.
global_placement -skip_nesterov_place
set feature_file [make_result_file ml_placement_parasitics_features.csv]
set prediction_file [make_result_file ml_placement_parasitics_predictions.csv]
set report_file [make_result_file ml_placement_parasitics_report.csv]
write_ml_net_features \
  -file $feature_file \
  -feature_set_id est-placement-parasitics

set input [open $feature_file r]
set rows {}
set feature_set_id ""
set dbu_per_micron ""
while { [gets $input line] >= 0 } {
  if { [regexp {^# feature_set_id=(.*)$} $line match value] } {
    set feature_set_id $value
  } elseif { [regexp {^# dbu_per_micron=(.*)$} $line match value] } {
    set dbu_per_micron $value
  } elseif { ![string match "#*" $line] && ![string match "net_id,*" $line] } {
    lappend rows $line
  }
}
close $input

set eligible_count 0
set output [open $prediction_file w]
puts $output "# openroad_ml_predictions_v1"
puts $output "# feature_set_id=$feature_set_id"
puts $output "# dbu_per_micron=$dbu_per_micron"
puts $output "# expected_net_count=[llength $rows]"
foreach row $rows {
  set fields [split $row ,]
  if { [lindex $fields 5] > 0 } {
    incr eligible_count
  }
}
puts $output "# eligible_net_count=$eligible_count"
puts $output "net_id,eligible,predicted_wirelength_dbu"
foreach row $rows {
  set fields [split $row ,]
  set eligible [expr {[lindex $fields 5] > 0}]
  set prediction [expr {$eligible ? 1.0e12 : ""}]
  puts $output "[lindex $fields 0],$eligible,$prediction"
}
close $output

estimate_parasitics -placement \
  -ml_wirelength_predictions $prediction_file \
  -ml_wirelength_alpha 0.5 \
  -ml_wirelength_ratio_limits {0.5 2.0} \
  -ml_wirelength_report $report_file

set report [open $report_file r]
gets $report header
set applied 0
while { [gets $report row] >= 0 } {
  set fields [split $row ,]
  if { [lindex $fields 11] eq "applied" } {
    incr applied
    if { abs([lindex $fields 9] - 1.5) > 1.0e-9 } {
      error "expected effective parasitic scale 1.5, got [lindex $fields 9]"
    }
    if { abs([lindex $fields 10] - 1.5 * [lindex $fields 6]) > 1.0e-3 } {
      error "effective wirelength does not match the applied scale"
    }
  }
}
close $report
if { $applied == 0 } {
  error "expected at least one ML-scaled placement parasitic net"
}
clear_ml_placement_parasitics
puts pass
