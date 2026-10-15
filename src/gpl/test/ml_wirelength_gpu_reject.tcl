# Verify the GPU runtime rejects ML wirelength injection explicitly.
source helpers.tcl
source ml_wirelength_helpers.tcl
setup_ml_simple01

lassign [ml_test_files ml-gpu-reject equal] prediction_file report_file
set failed [catch {
  global_placement -init_density_penalty 0.01 -skip_initial_place \
    -ml_wirelength_predictions $prediction_file \
    -ml_wirelength_report $report_file
} message]
if { !$failed || ![string match "*GPL-1108*" $message] } {
  error "GPU runtime did not reject ML wirelength injection: $message"
}
puts pass
