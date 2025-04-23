import sys
import os
import argparse
import pdb

from launch import *
from diff   import *

def parse_arguments():
    parser = argparse.ArgumentParser("FoxTest")
    parser.add_argument('-base',      type = str,  default='default',       help='the command which is used for baseline')
    parser.add_argument('-enhanced',  type = str,  default=None,            help='the command which is used for test')
    parser.add_argument('-case_set',  type = str,  default='all',           help='the case set used for regression')
    parser.add_argument('-formal',    type = int,  default=0,               help='enable formal verification or not')
    parser.add_argument('-no_diff',   type = int,  default=0,               help='disable comparison')
    parser.add_argument('-post',      type = str,  default=None,            help='abc commands used for post-run')

    args = parser.parse_args()

    return args

args = parse_arguments()

if __name__ == "__main__":
    try:
        failed_case, log_file_set = LaunchFoxSynTest(args)
        if failed_case is not None:
            print("Testcase {} failed, please check\n".format(failed_case))
            sys.exit(1)

        if args.no_diff == 0:
            CompareAndPrint(log_file_set)
        else:
            print("Nice work! all testcases passed tests :)\n")

    except KeyboardInterrupt:
        print("killed by Ctrl-C")
        sys.exit(1)
