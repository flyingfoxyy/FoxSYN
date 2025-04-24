
import os
import pdb
import re

class NetlsitInfo():
    def __init__(self, log_file :str):
        self.num_node    = 0
        self.num_level   = 0
        self.num_edge    = 0
        self.num_input   = 0
        self.num_output  = 0
        self.num_lat     = 0
        self.num_cube    = 0
        self.runtime     = 0
        self.num_by_size = 0
        self.case_name   = None

        if log_file is None or len(log_file) == 0:
            return

        # parse the log
        with open(log_file, "r", encoding="utf-8") as file:
            for line in file:
                if "i/o" in line:
                    data_part = line.split(":", 1)[1].strip()
                    pattern = r'([\w/]+)\s*=\s*([\d/ ]+)'
                    for key, value in re.findall(pattern, data_part):
                        value = value.strip()
                        if key == 'i/o':
                            self.num_input, self.num_output = map(int, map(str.strip, value.split('/')))
                        elif key == 'lat':
                            self.num_lat = int(value)
                        elif key == 'nd':
                            self.num_node = int(value)
                        elif key == 'edge':
                            self.num_edge = int(value)
                        elif key == 'cube':
                            self.num_cube = int(value)
                        elif key == 'lev':
                            self.num_level = int(value)

def CompareAndPrint(base_log, sota_log):
    assert(base_log is None or len(base_log) == len(sota_log))

    table = []

    with_compare = base_log is not None

    if base_log is None:
        table.append(["CaseName", "Area", "Delay", "Edge", "RT"])
    else:
        table.append(["CaseName", "Area", "Delay", "Edge", "RT",
                    "Area", "ratio", "Delay", "ratio", "Edge", "ratio", "RT", "ratio"])

    base_nl_info = []
    sota_nl_info = []

    if with_compare is True:
        for log in base_log:
            base_nl_info.append(NetlsitInfo(log))

    for log in sota_log:
        sota_nl_info.append(NetlsitInfo(log))

    num_case = len(sota_log)

    idx = 0
    for info in sota_nl_info:
        row = ["name"]

        base_info = NetlsitInfo(None)
        if with_compare:
            base_info = base_nl_info[idx]
            row.append(base_info.num_node)
            row.append(base_info.num_level)
            row.append(base_info.num_edge)
            row.append(base_info.runtime)

        row.append(info.num_node)
        if with_compare:
            row.append(info.num_node / base_info.num_node)
        row.append(info.num_level)
        if with_compare:
            row.append(info.num_level / base_info.num_level)
        row.append(info.num_edge)
        if with_compare:
            row.append(info.num_edge / base_info.num_edge)
        row.append(info.runtime)
        if with_compare:
            row.append(info.runtime / base_info.runtime)

        table.append(row)

        idx += 1

    # print the table
    if with_compare:
        for row in table:
            print("{:<10} {:<6} {:<6} {:<6} {:<6} {:<6} {:<6} {:<6} {:<6} {:<6} {:<6} {:<6} {:<6}".format(*row))
    else:
        for row in table:
            print("{:<10} {:<6} {:<6} {:<6} {:<6}".format(*row))
