proc write_ml_predictions { feature_file prediction_file mode } {
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

  set output [open $prediction_file w]
  puts $output "# openroad_ml_predictions_v1"
  puts $output "# feature_set_id=$feature_set_id"
  puts $output "# dbu_per_micron=$dbu_per_micron"
  puts $output "# expected_net_count=[llength $rows]"

  set eligible_count 0
  foreach row $rows {
    set fields [split $row ,]
    if { [lindex $fields 5] > 0 } {
      incr eligible_count
    }
  }
  puts $output "# eligible_net_count=$eligible_count"
  puts $output "net_id,eligible,predicted_wirelength_dbu"

  set eligible_index 0
  foreach row $rows {
    set fields [split $row ,]
    set net_id [lindex $fields 0]
    set hpwl [lindex $fields 5]
    set eligible [expr {$hpwl > 0}]
    set prediction ""
    if { $eligible } {
      if { $mode eq "missing_first" && $eligible_index == 0 } {
        set prediction ""
      } elseif { $mode eq "nonuniform" } {
        set ratio [expr {$eligible_index % 2 == 0 ? 0.5 : 2.0}]
        set prediction [expr {$hpwl * $ratio}]
      } else {
        set prediction $hpwl
      }
      incr eligible_index
    }
    puts $output "$net_id,$eligible,$prediction"
  }
  close $output
}

proc ml_test_files { name mode } {
  set feature_file [make_result_file "$name-features.csv"]
  set prediction_file [make_result_file "$name-predictions.csv"]
  set report_file [make_result_file "$name-report.csv"]
  write_ml_net_features -file $feature_file -feature_set_id $name
  write_ml_predictions $feature_file $prediction_file $mode
  return [list $prediction_file $report_file]
}

proc assert_unit_applied_weights { report_file } {
  set report [open $report_file r]
  set applied 0
  gets $report header
  while { [gets $report row] >= 0 } {
    set fields [split $row ,]
    if { [lindex $fields 10] eq "applied" } {
      incr applied
      if { abs([lindex $fields 9] - 1.0) > 1e-6 } {
        error "expected unit ML custom weight, got [lindex $fields 9]"
      }
    }
  }
  close $report
  if { $applied == 0 } {
    error "expected at least one applied ML weight"
  }
}

proc assert_report_uses_exported_hpwl { feature_file report_file } {
  set features [open $feature_file r]
  while { [gets $features row] >= 0 } {
    if { [string match "#*" $row] || [string match "net_id,*" $row] } {
      continue
    }
    set fields [split $row ,]
    set exported_hpwl([lindex $fields 0]) [lindex $fields 5]
  }
  close $features

  set report [open $report_file r]
  gets $report header
  while { [gets $report row] >= 0 } {
    set fields [split $row ,]
    set net_id [lindex $fields 0]
    set applied_hpwl [lindex $fields 5]
    if { ![info exists exported_hpwl($net_id)] } {
      error "report contains unknown ML net id $net_id"
    }
    if { $exported_hpwl($net_id) != $applied_hpwl } {
      error "net $net_id used HPWL $applied_hpwl instead of exported HPWL $exported_hpwl($net_id)"
    }
  }
  close $report
}

proc write_current_hpwl_predictions { template_file report_file output_file } {
  set report [open $report_file r]
  gets $report header
  while { [gets $report row] >= 0 } {
    set fields [split $row ,]
    set current_hpwl([lindex $fields 0]) [lindex $fields 5]
  }
  close $report

  set template [open $template_file r]
  set output [open $output_file w]
  while { [gets $template line] >= 0 } {
    if { [string match "#*" $line] || [string match "net_id,*" $line] } {
      puts $output $line
      continue
    }
    set fields [split $line ,]
    set net_id [lindex $fields 0]
    set eligible [lindex $fields 1]
    set prediction ""
    if { $eligible && $current_hpwl($net_id) > 0 } {
      set prediction $current_hpwl($net_id)
    }
    puts $output "$net_id,$eligible,$prediction"
  }
  close $template
  close $output
}

proc setup_ml_simple01 { } {
  read_lef ./nangate45.lef
  read_def ./simple01.def
}
