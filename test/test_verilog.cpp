#include <iostream>
#include "supper/map.hpp"

using namespace fox::supper;

// Create a test class that extends graph_t to access protected members
class TestGraph : public graph_t {
public:
    TestGraph(uint max_node_num, uint num_pi = 0, uint num_po = 0)
        : graph_t(max_node_num, num_pi, num_po) {}
        
    // Method to build a test graph
    void build_test_graph() {
        // Node 0 is always ONE/constant
        _nodes.emplace_back(node_type_t::ONE);
        
        // Nodes 1, 2 are PIs
        _nodes.emplace_back(node_type_t::PI);
        _pi.push_back(1);
        
        _nodes.emplace_back(node_type_t::PI);
        _pi.push_back(2);
        
        // Node 3 is a logic node (AND gate) with inputs from nodes 1 and 2
        _nodes.emplace_back(node_type_t::LOGIC, Lit(1, 0), Lit(2, 1));
        
        // Node 4 is PO with input from node 3
        _nodes.emplace_back(node_type_t::PO, Lit(3, 0));
        _po.push_back(4);
    }
};

int main() {
    // Create a simple graph with 2 PIs, 1 PO, and 1 logic node
    TestGraph g(10, 2, 1);
    
    // Build the test graph
    g.build_test_graph();
    
    // Print some information about the graph
    std::cout << "Graph structure:\n";
    std::cout << "Number of nodes: " << g.num_nodes() << "\n";
    std::cout << "Number of PIs: " << g.num_pi() << "\n";
    std::cout << "Number of POs: " << g.num_po() << "\n";
    std::cout << "Number of logic nodes: " << g.num_logic() << "\n";
    
    // Export the graph to Verilog
    if (g.write_to_verilog("test_circuit.v")) {
        std::cout << "Successfully wrote Verilog netlist to test_circuit.v" << std::endl;
    } else {
        std::cerr << "Failed to write Verilog netlist" << std::endl;
        return 1;
    }
    
    return 0;
}
