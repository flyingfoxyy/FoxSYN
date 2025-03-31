import os
import shutil

# Globale Varibales
CaseRoot = 'SimpleCircuits/circuits/'
CoreCmd  = 'st; opt; {}; ps'

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
    print(f"launch command: {cmd}")
    ret = os.system(cmd)
    if ret != 0:
        print(f"command failed: {cmd} (ret code: {ret})", file=sys.stderr)
    return ret

def execute_commands_parallel(commands, processes=None):
    with Pool(processes=processes) as pool:
        results = pool.map(run_command, commands)

    # check if some commands failed
    if any(r != 0 for r in results):
        print("some commands failed", file=sys.stderr)
        sys.exit(1)
    # print("all commands finished")

def ParallelRunAbc(set_name, case_names, cmd_list1, cmd_list2):
    assert len(case_names) == len(cmd_list1) and len(cmd_list1) == len(cmd_list2)

    dir_set = []
    idx = 0
    for case in case_names:
        path = "./" + set_name + "/" + case
        if os.path.exists(path):
            shutil.rmtree(path)
        os.makedirs(path, exist_ok=True)
        dir_set.append(path)
        cmd_list1[idx] = "cd " + path + "; " + cmd_list1[idx] + " > abc.log"
        cmd_list2[idx] = "cd " + path + "; " + cmd_list2[idx] + " > fox.log"
        idx += 1
    
    execute_commands_parallel(set(cmd_list1))
    execute_commands_parallel(set(cmd_list2))
    
    



def RunAbcCommands(circuit_set, abc_cmd :str, fox_cmd :str):
    set_abc_logs = {}
    set_fox_logs = {}

    cmd_pre_fix = '../release/FoxSYN -c '

    abc_core_cmd = CoreCmd.format(abc_cmd)
    fox_core_cmd = CoreCmd.format(fox_cmd)

    for set_name in circuit_set:
        if set_name != "mcnc" and set_name != "EPFL" and set_name != "opencores" and set_name != "vtr":
            print("unknown benchmark set " + set_name)
            exit(1)
        set = BenchmarkSet(set_name)

        case_name_set = set.GetCaseNames()
        case_path_set = set.GetCasePaths()

        launch_abc_cmds = []
        launch_fox_cmds = []

        idx = 0
        for caes in case_name_set:
            launch_abc_cmds.append(cmd_pre_fix + "\"read " + case_path_set[idx] + "; " + abc_core_cmd)
            launch_fox_cmds.append(cmd_pre_fix + "\"read " + case_path_set[idx] + "; " + fox_core_cmd)

        # create directoies for each case
        dir_set = ParallelRunAbc(set_name, case_name_set, launch_abc_cmds, launch_fox_cmds)
        
        abc_log_name = "run_" + set_name + "_abc.log"
        fox_log_name = "run_" + set_name + "_fox.log"

        for dir in dir_set:
                        






    
        
        

