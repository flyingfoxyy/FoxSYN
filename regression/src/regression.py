import sys
import os
import argparse
import pdb

from launch import *
from diff   import *

def parse_arguments():
    parser = argparse.ArgumentParser("FoxTest")
    parser.add_argument('-base',      type = str,  default=None,   help='the command which is used for base')
    parser.add_argument('-sota',      type = str,  default=None,   help='the command which is used for sota')
    parser.add_argument('-pre_opt',   type = str,  default="opt",  help='command used for post-processing')
    parser.add_argument('-case_set',  type = str,  default='all',  help='the case set used for regression')
    parser.add_argument('-formal',    type = int,  default=0,      help='enable formal verification or not')
    parser.add_argument('-post',      type = str,  default=None,   help='command used for post-processing')

    args = parser.parse_args()

    return args

args = parse_arguments()

if __name__ == "__main__":
    try:
        if args.base is None and args.sota is None:
            print("-base and -sota both are not set\n")
            sys.exit(1)

        base_log, sota_log = LaunchFoxSynTest(args)

        CompareAndPrint(base_log, sota_log)

    except KeyboardInterrupt:
        print("killed by Ctrl-C")
        sys.exit(1)
