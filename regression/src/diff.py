import os
import pdb

def CompareAndPrint(log_file_sets :list):
    assert(len(log_file_sets) == 2 and len(log_file_sets[1]) > 0)
    for log_file_path in log_file_sets[1]:
        print("log file path: " + log_file_path)
    
    