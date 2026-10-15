# Verify alpha zero preserves the reference placement and applies unit weights.
source helpers.tcl
source ml_wirelength_helpers.tcl
setup_ml_simple01

lassign [ml_test_files ml-alpha0 equal] prediction_file report_file
set feature_file [make_result_file "ml-alpha0-features.csv"]
global_placement -init_density_penalty 0.01 -skip_initial_place \
  -ml_wirelength_predictions $prediction_file \
  -ml_wirelength_alpha 0 \
  -ml_wirelength_report $report_file

set def_file [make_result_file ml_wirelength_alpha0.def]
write_def $def_file
diff_file $def_file simple01.defok
assert_unit_applied_weights $report_file
assert_report_uses_exported_hpwl $feature_file $report_file
puts pass
