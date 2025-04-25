@echo off

%1 -bt ue4 --batch_mode --log_to_console false --output_env_json_file %2 --commit_suicide --tool_chain_json_file %3 -a "%4" --controller_no_wait --controller_remain_time 120 --auto_resource_mgr --res_idle_secs_for_free 1800 --send_cork