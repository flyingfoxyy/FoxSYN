#include <iostream>
#include <string>
#include "supper/map.hpp"

using namespace fox::supper;

// Create a test class that inherits from graph_t to access protected members
class TestGraph : public graph_t {
public:
    TestGraph(uint max_node_num, uint num_pi = 0, uint num_po = 0)
        : graph_t(max_node_num, num_pi, num_po) {}
    
    void create_test_structure() {
        // Node 0: ONE (constant)
        _nodes.emplace_back(node_type_t::ONE);
        
        // Node 1: PI
        _nodes.emplace_back(node_type_t::PI);
        _pi.push_back(1);
        
        // Node 2: PI
        _nodes.emplace_back(node_type_t::PI);
        _pi.push_back(2);
        
        // Node 3: Logic node with input from node 1 (regular) and node 2 (inverted)
        _nodes.emplace_back(node_type_t::LOGIC, 
                          Lit(1, 0),   // regular input from node 1
                          Lit(2, 1));  // inverted input from node 2
        
        // Node 4: PO with input from node 3
        _nodes.emplace_back(node_type_t::PO, Lit(3, 0));
        _po.push_back(4);
    }
};

// Simple standalone test program to demonstrate the use of to_dot function
int main(int argc, char* argv[]) {
    // Create a test graph with 10 nodes (max), 2 PIs, and 1 PO
    TestGraph g(10, 2, 1);
    
    // Create the test graph structure
    g.create_test_structure();
    
    // Output the graph to a DOT file
    std::string dot_file = "test_graph.dot";
    if (g.to_dot(dot_file)) {
        std::cout << "Successfully wrote DOT file to " << dot_file << std::endl;
        std::cout << "You can visualize this file using tools like Graphviz:" << std::endl;
        std::cout << "  $ dot -Tpng " << dot_file << " -o test_graph.png" << std::endl;
        return 0;
    } else {
        std::cerr << "Failed to write DOT file" << std::endl;
        return 1;
    }
}
