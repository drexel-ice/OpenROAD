# Prepare reference HPWL data for the equal-prediction integration test.
source helpers.tcl
source ml_wirelength_helpers.tcl
setup_ml_simple01

write_ml_net_features \
  -file $::env(OR_ML_EQUAL_FEATURES) \
  -feature_set_id ml-equal
write_ml_predictions \
  $::env(OR_ML_EQUAL_FEATURES) \
  $::env(OR_ML_EQUAL_PREDICTIONS) \
  equal
global_placement -init_density_penalty 0.01 -skip_initial_place \
  -ml_wirelength_predictions $::env(OR_ML_EQUAL_PREDICTIONS) \
  -ml_wirelength_alpha 0 \
  -ml_wirelength_report $::env(OR_ML_EQUAL_REPORT)
