#include <iostream>
#include <fstream>
#include <limits>

#include "graph.hpp"
#include "base/abc/abc.h"

namespace fox::supper {
uint
graph_t::add_const1()
{
    Assert(_nodes.empty());
    _nodes.emplace_back(node_type_t::ONE);
    return _nodes.size() - 1;
}

uint
graph_t::add_pi(std::string name)
{
    _nodes.emplace_back(node_type_t::PI);
    uint id = _nodes.size() - 1;
    _pi.push_back(id);
    _pi_names.push_back(std::move(name));
    return id;
}

uint
graph_t::add_po(std::string name)
{
    _nodes.emplace_back(node_type_t::PO);
    uint id = _nodes.size() - 1;
    _po.push_back(id);
    _po_names.push_back(std::move(name));
    return id;
}

uint
graph_t::add_lut(std::vector<Lit> fanins, word truth)
{
    _nodes.emplace_back(node_type_t::LOGIC, std::move(fanins), truth);
    return _nodes.size() - 1;
}

void
graph_t::set_po_fanin(uint po_idx, Lit fanin)
{
    Assert(po_idx < _po.size());
    _nodes[_po[po_idx]].set_fanin(fanin);
}

bool
graph_t::is_topologically_sorted() const
{
    // For a logic node, all of its fanins should be stored before it.
    ForEachGraphLogicNode(*this) {
        const auto &node = _nodes[idx];
        for (int i = 0; i != node.size(); ++i)
            if (node[i].id() > idx)
                return false;
    }
    return true;
}

void
graph_t::report(std::ostream &os)
{
    os << "graph stats: ";
    os << "PI "    << num_pi()    << "\t";
    os << "PO "    << num_po()    << "\t";
    os << "LOGIC " << num_logic() << "\t";
    os << "\n";
}

void *
graph_t::to_abc_ntk()
{
    Abc_Ntk_t *ntk = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    std::vector<Abc_Obj_t *> cache(num_nodes(), nullptr);

    Abc_Obj_t *const1 = nullptr;
    Abc_Obj_t *const0 = nullptr;

    auto get_const1 = [&]() -> Abc_Obj_t * {
        if (!const1)
            const1 = Abc_NtkCreateNodeConst1(ntk);
        return const1;
    };
    auto get_const0 = [&]() -> Abc_Obj_t * {
        if (!const0)
            const0 = Abc_NtkCreateNodeConst0(ntk);
        return const0;
    };
    auto maybe_invert = [&](Abc_Obj_t *obj, bool sign) -> Abc_Obj_t * {
        return sign ? Abc_NtkCreateNodeInv(ntk, obj) : obj;
    };

    cache[0] = get_const1();

    // A complemented PO fanin must not be materialized as an inverter node: that costs
    // one node and one logic level on the PO path. Mirror the reference exporter
    // (agdmap.cpp agdmapToAbcLogic) -- absorb the inversion into the driver's truth
    // table when this PO is the driver's only user, otherwise leave the driver alone and
    // mark the PO fanin complemented, which ABC logic networks represent natively.
    std::vector<uint> ref_count(num_nodes(), 0);
    ForEachGraphLogicNode(*this) {
        const node_t &node = _nodes[idx];
        for (uint i = 0; i < node.size(); ++i)
            ++ref_count[node[i].id()];
    }
    ForEachGraphPo(*this) {
        const node_t &node = get_po(idx);
        if (node.size())
            ++ref_count[node[0].id()];
    }

    std::vector<bool> invert_truth(num_nodes(), false);
    ForEachGraphPo(*this) {
        const node_t &node = get_po(idx);
        if (node.size() == 0 || !node[0].sign())
            continue;
        const uint driver = node[0].id();
        if (_nodes[driver].is_logic() && ref_count[driver] == 1)
            invert_truth[driver] = true;
    }

    ForEachGraphPi(*this) {
        Abc_Obj_t *pi = Abc_NtkCreatePi(ntk);
        std::string fallback = "pi" + std::to_string(idx);
        const std::string &stored = (idx < _pi_names.size() && !_pi_names[idx].empty()) ? _pi_names[idx] : fallback;
        Abc_ObjAssignName(pi, const_cast<char *>(stored.c_str()), nullptr);
        cache[pi_id(idx)] = pi;
    }

    ForEachGraphLogicNode(*this) {
        const node_t &node = _nodes[idx];
        Assert(node.size() <= 6);
        word truth = node.has_truth() ? node.truth() :
            (node.size() == 2 ? 0x8888888888888888ULL : 0xAAAAAAAAAAAAAAAAULL);
        if (invert_truth[idx])
            truth = ~truth;

        const word mask = node.size() >= 6 ? ~0ULL : ((1ULL << (1u << node.size())) - 1ULL);
        if ((truth & mask) == 0) {
            cache[idx] = get_const0();
            continue;
        }
        if ((truth & mask) == mask) {
            cache[idx] = get_const1();
            continue;
        }

        Abc_Obj_t *lut = Abc_NtkCreateObj(ntk, ABC_OBJ_NODE);
        lut->pData = Abc_SopRegister(
            (Mem_Flex_t *)ntk->pManFunc,
            Abc_SopCreateFromTruth((Mem_Flex_t *)ntk->pManFunc, node.size(), (unsigned *)&truth));

        for (uint i = 0; i < node.size(); ++i) {
            Lit fanin = node[i];
            Assert(fanin.id() < cache.size());
            Assert(cache[fanin.id()]);
            Abc_ObjAddFanin(lut, maybe_invert(cache[fanin.id()], fanin.sign()));
        }
        cache[idx] = lut;
    }

    ForEachGraphPo(*this) {
        Abc_Obj_t *po = Abc_NtkCreatePo(ntk);
        std::string fallback = "po" + std::to_string(idx);
        const std::string &stored = (idx < _po_names.size() && !_po_names[idx].empty()) ? _po_names[idx] : fallback;
        Abc_ObjAssignName(po, const_cast<char *>(stored.c_str()), nullptr);

        const node_t &node = get_po(idx);
        if (node.size() == 0) {
            Abc_ObjAddFanin(po, get_const0());
            continue;
        }

        Lit fanin = node[0];
        Assert(fanin.id() < cache.size());
        Assert(cache[fanin.id()]);
        Abc_ObjAddFanin(po, cache[fanin.id()]);
        if (fanin.sign() && !invert_truth[fanin.id()])
            Abc_ObjSetFaninC(po, 0);
    }

    if (const1 && Abc_ObjFanoutNum(const1) == 0)
        Abc_NtkDeleteObj(const1);
    if (const0 && Abc_ObjFanoutNum(const0) == 0)
        Abc_NtkDeleteObj(const0);

    return static_cast<void *>(ntk);
}

bool
graph_t::to_dot(const std::string &path) const
{
    // Open file for writing
    FILE *fp = fopen(path.c_str(), "w");
    if (!fp) {
        std::cerr << "Failed to open file: " << path << std::endl;
        return false;
    }

    // Write DOT file header
    fprintf(fp, "digraph G {\n");
    
    // Set up graph properties
    fprintf(fp, "  graph [rankdir=TB];\n");
    fprintf(fp, "  node [shape=ellipse, style=filled, fillcolor=lightgrey];\n");
    
    // Define nodes
    for (int idx = begin(); idx != end(); ++idx) {
        const auto &node = _nodes[idx];
        if (node.null()) continue;
        
        if (node.is_pi()) {
            // PI nodes as downward triangles
            fprintf(fp, "  n%d [shape=invtriangle, fillcolor=lightblue, label=\"PI %d\"];\n", idx, idx);
        } else if (node.is_po()) {
            // PO nodes as upward triangles
            fprintf(fp, "  n%d [shape=triangle, fillcolor=lightgreen, label=\"PO %d\"];\n", idx, idx);
        } else if (node.is_logic()) {
            // Logic nodes as ellipses with IDs
            fprintf(fp, "  n%d [shape=ellipse, fillcolor=white, label=\"%d\"];\n", idx, idx);
        }
    }
    
    // Define edges
    for (int idx = begin(); idx != end(); ++idx) {
        const auto &node = _nodes[idx];
        if (node.null()) continue;
        
        for (uint i = 0; i < node.size(); ++i) {
            Lit fanin = node[i];
            // If sign is true (inverted input), use dashed line, otherwise solid
            const char* style = fanin.sign() ? "dashed" : "solid";
            fprintf(fp, "  n%d -> n%d [style=%s];\n", fanin.id(), idx, style);
        }
    }
    
    // Close the graph
    fprintf(fp, "}\n");
    
    // Close the file
    fclose(fp);
    
    return true;
}

bool
graph_t::write_to_verilog(const std::string &path) const
{
    // Open file for writing
    FILE *fp = fopen(path.c_str(), "w");
    if (!fp) {
        std::cerr << "Failed to open file: " << path << std::endl;
        return false;
    }
    
    // Generate a module name based on the file path
    std::string module_name = "circuit";
    size_t last_slash = path.find_last_of("/\\");
    size_t last_dot = path.find_last_of(".");
    if (last_slash != std::string::npos && last_dot != std::string::npos) {
        module_name = path.substr(last_slash + 1, last_dot - last_slash - 1);
    }
    
    // Write Verilog module header
    fprintf(fp, "// Verilog netlist generated by FoxSYN\n");
    fprintf(fp, "module %s(\n", module_name.c_str());
    
    // Write module ports
    // First, declare all inputs (PIs)
    if (num_pi() > 0) {
        fprintf(fp, "    // Primary inputs\n");
        for (uint i = 0; i < num_pi(); ++i) {
            fprintf(fp, "    input wire pi%d%s\n", i, (i == num_pi() - 1 && num_po() == 0) ? "" : ",");
        }
    }
    
    // Then, declare all outputs (POs)
    if (num_po() > 0) {
        fprintf(fp, "    // Primary outputs\n");
        for (uint i = 0; i < num_po(); ++i) {
            fprintf(fp, "    output wire po%d%s\n", i, (i == num_po() - 1) ? "" : ",");
        }
    }
    
    fprintf(fp, ");\n\n");
    
    // Declare internal wires for logic nodes
    if (num_logic() > 0) {
        fprintf(fp, "    // Internal wires\n");
        for (int idx = begin(); idx != end(); ++idx) {
            const auto &node = _nodes[idx];
            if (!node.null() && node.is_logic()) {
                fprintf(fp, "    wire n%d;\n", idx);
            }
        }
        fprintf(fp, "\n");
    }
    
    // Write the logic for each node
    // Process PI nodes - assign to nodes
    for (uint i = 0; i < num_pi(); ++i) {
        uint pi_id = _pi[i];
        fprintf(fp, "    // PI node %d\n", pi_id);
        fprintf(fp, "    wire n%d = pi%d;\n", pi_id, i);
    }
    fprintf(fp, "\n");
    
    // Process logic nodes
    fprintf(fp, "    // Logic nodes\n");
    for (int idx = begin(); idx != end(); ++idx) {
        const auto &node = _nodes[idx];
        if (!node.null() && node.is_logic()) {
            fprintf(fp, "    // Node %d\n", idx);
            
            // For a 2-input logic node (AND gate with possible inversions)
            if (node.size() == 2) {
                Lit fanin0 = node[0];
                Lit fanin1 = node[1];
                
                // Handle potential inversions on inputs
                const char* in0 = fanin0.sign() ? "~" : "";
                const char* in1 = fanin1.sign() ? "~" : "";
                
                fprintf(fp, "    assign n%d = %sn%d & %sn%d;\n", 
                        idx, in0, fanin0.id(), in1, fanin1.id());
            }
            // Handle nodes with single fanin (e.g., buffers or inverters)
            else if (node.size() == 1) {
                Lit fanin0 = node[0];
                const char* in0 = fanin0.sign() ? "~" : "";
                fprintf(fp, "    assign n%d = %sn%d;\n", idx, in0, fanin0.id());
            }
        }
    }
    fprintf(fp, "\n");
    
    // Process PO nodes
    for (uint i = 0; i < num_po(); ++i) {
        uint po_id = _po[i];
        const auto &node = _nodes[po_id];
        
        fprintf(fp, "    // PO node %d\n", po_id);
        
        if (node.size() == 1) {
            Lit fanin = node[0];
            const char* in_sign = fanin.sign() ? "~" : "";
            fprintf(fp, "    assign po%d = %sn%d;\n", i, in_sign, fanin.id());
        }
        else {
            // If PO has no fanin, assign it a constant 0
            fprintf(fp, "    assign po%d = 1'b0; // No fanin\n", i);
        }
    }
    
    // Close the module
    fprintf(fp, "endmodule\n");
    
    // Close the file
    fclose(fp);
    
    return true;
}

}
