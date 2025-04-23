import os
import pdb

def CompareAndPrint(base_log, enhanced_log):
    # assert(len(log_file_sets) == 2 and len(log_file_sets[1]) > 0)
    for log_file_path in enhanced_log:
        print("log file path: " + log_file_path)
    
    