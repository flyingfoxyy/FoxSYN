import os
import shutil
import sys
import pdb

from multiprocessing import Pool

# Globale Varibales
CaseRoot = 'SimpleCircuits/circuits/'
CoreCmd  = 'st; resyn; resyn2; {}; ps'

# Runtime flags
PostCmd     = None
Formal      = None
CurrentPath = None

class BenchmarkSet():
    def __init__(self, name: str):
        self.name = name
        self.case_name = []
        self.case_path = []

        set_path = CaseRoot + self.name
        file_list = set_path + "/list"
        with open(file_list, "r", encoding="utf-8") as file:
            for line in file:
                self.case_name.append(line.rpartition('.')[0])
                self.case_path.append(set_path + '/' + line)

    def GetCaseNames(self):
        return self.case_name
    
    def GetCasePaths(self):
        return self.case_path

def run_command(cmd):
    # print(f"launch command: {cmd}")
    ret = os.system(cmd)
    if ret != 0:
        print(f"command failed: {cmd} (ret code: {ret})", file=sys.stderr)
    return ret

def execute_commands_parallel(commands, processes=None):
    with Pool(processes=processes) as pool:
        results = pool.map(run_command, commands)

    # check if some commands failed
    if any(r != 0 for r in results):
        # print("some commands failed", file=sys.stderr)
        sys.exit(1)

def RunAbcCommands(circuit_set, cmd :str, log :str):
    if not os.path.exists("rundir"):
        os.makedirs("rundir")

    cmd_pre_fix = '../release/FoxSYN -c '
    core_cmd = CoreCmd.format(cmd)

    log_file_set = []

    for set_name in circuit_set:
        if set_name != "mcnc" and set_name != "EPFL" and set_name != "opencores" and set_name != "vtr":
            print("unknown benchmark set " + set_name)
            exit(1)

        caset_set = BenchmarkSet(set_name)

        # get the name and detail path for each case
        case_name_set = caset_set.GetCaseNames()
        case_path_set = caset_set.GetCasePaths()

        complete_cmds = []

        idx = 0
        for case in case_name_set:
            abc_cmd_str = cmd_pre_fix + "\"read " + case_path_set[idx] + "; " + core_cmd + "\""
            log_path = path = "./rundir/" + case + "/"
            if not os.path.exists(log_path):
                os.makedirs(log_path)
            log_file = log_path + log
            complete_cmds.append(abc_cmd_str + " > " + log_file)
            log_file_set.append(log_file)
            idx += 1

        execute_commands_parallel(set(complete_cmds))

    return log_file_set

def LaunchFoxSynTest(args):
    # initialize global variables
    CurrentPath = os.getcwd()
    if args.formal is True:
        Formal = "cec"

    circuit_set = []
    log_set_enhanced = []
    log_set_base = []

    if args.enhanced is None:
        print("Error: no enhanced commandn\n")
        sys.exit(1)

    if args.case_set == "all":
        circuit_set = ["EPFL", "mcnc", "opencores", "vtr"]
    else:
        circuit_set.append(args.case_set)

    # launch enhanced command first
    log_set_enhanced = RunAbcCommands(circuit_set, args.enhanced, 'enhanced.log')

    if args.base != "default":
        log_set_base = RunAbcCommands(circuit_set, args.base, 'base.log')
        return log_set_base, log_set_enhanced
    else:
        return None, log_set_enhanced

