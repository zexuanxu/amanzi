import os
import sys
import numpy as np
import pytest


def test_butler_pod_2d():

    try:
        sys.path.append('../../../../../../tools/amanzi_xml')
    except:
        pass

    import run_amanzi_standard
    input_file =os.path.join("amanzi_butler_pod_2d.xml")
    run_dir = "amanzi-output"
    golden_dir = "golden_output"
    
    try:
        sys.path.append('../../../../../../tools/testing')
    except:
        pass

    import compare_observation_results as comp_obs
    
    CWD = os.getcwd()
    try:
        run_amanzi_standard.run_amanzi(input_file, 1, [input_file], run_dir)
        obs_name = "drawdown"        
        run_obs_coord, run_obs_val = comp_obs.get_observation_data(run_dir + "/observations.out", obs_name)
        gold_obs_coord, gold_obs_val = comp_obs.get_observation_data(golden_dir + "/observations.out", obs_name)

        errors = comp_obs.compute_error_norms(run_obs_coord, run_obs_val, gold_obs_coord, gold_obs_val)

        for region, error in errors.items():                
            assert error < 1e-5
        
    finally:
        os.chdir(CWD)
