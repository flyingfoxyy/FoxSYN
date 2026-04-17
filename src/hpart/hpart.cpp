#include "hpart.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "base/abc/abc.h"

ABC_NAMESPACE_IMPL_START

namespace fox::hpart {

namespace {

struct Hypergraph {
    std::vector<Abc_Obj_t *> vertices;
    std::vector<std::vector<int>> edges;
};

struct VertexTraits {
    bool saw_latch = false;
    bool warned_unknown = false;
};

struct TempDir {
    std::filesystem::path path;
    bool valid = false;

    ~TempDir()
    {
        if ( !valid )
            return;
        std::error_code ec;
        std::filesystem::remove_all( path, ec );
    }
};

const char *PartitionFileSuffix = ".part.";

std::string ShellQuote( const std::string &value )
{
    std::string quoted = "'";
    for ( char ch : value )
    {
        if ( ch == '\'' )
            quoted += "'\\''";
        else
            quoted += ch;
    }
    quoted += "'";
    return quoted;
}

std::filesystem::path FindExecutable( const char *name )
{
    if ( name == nullptr || name[0] == '\0' )
        return {};

    const std::string file_name = name;
    if ( file_name.find('/') != std::string::npos )
    {
        if ( access( file_name.c_str(), X_OK ) == 0 )
            return std::filesystem::path( file_name );
        return {};
    }

    const char *path_env = std::getenv( "PATH" );
    if ( path_env == nullptr )
        return {};

    std::stringstream ss( path_env );
    std::string dir;
    while ( std::getline( ss, dir, ':' ) )
    {
        if ( dir.empty() )
            continue;
        std::filesystem::path candidate = std::filesystem::path( dir ) / file_name;
        if ( access( candidate.c_str(), X_OK ) == 0 )
            return candidate;
    }
    return {};
}

TempDir CreateTempDir()
{
    TempDir temp_dir;
    std::filesystem::path template_path = std::filesystem::temp_directory_path() / "foxsyn_hpart_XXXXXX";
    std::string path_str = template_path.string();
    std::vector<char> buffer( path_str.begin(), path_str.end() );
    buffer.push_back( '\0' );

    char *created = mkdtemp( buffer.data() );
    if ( created == nullptr )
        return temp_dir;

    temp_dir.path = created;
    temp_dir.valid = true;
    return temp_dir;
}

bool IsHyperNode( Abc_Obj_t *pObj )
{
    return pObj != nullptr
        && ( Abc_ObjIsPi( pObj )
          || Abc_ObjIsPo( pObj )
          || Abc_ObjIsNode( pObj )
          || Abc_ObjIsLatch( pObj )
          || Abc_ObjType( pObj ) == ABC_OBJ_CONST1 );
}

bool IsCarrierNode( Abc_Obj_t *pObj )
{
    return pObj != nullptr
        && ( Abc_ObjIsPi( pObj )
          || Abc_ObjIsNode( pObj )
          || Abc_ObjIsLatch( pObj )
          || Abc_ObjType( pObj ) == ABC_OBJ_CONST1 );
}

bool ShouldTraverseInterconnect( Abc_Obj_t *pObj )
{
    return pObj != nullptr
        && ( Abc_ObjIsNet( pObj )
          || Abc_ObjIsBi( pObj )
          || Abc_ObjIsBo( pObj ) );
}

void WarnUnexpectedObject( Abc_Obj_t *pObj, VertexTraits &traits )
{
    if ( pObj == nullptr || traits.warned_unknown )
        return;
    if ( Abc_ObjIsNone( pObj ) || Abc_ObjIsNet( pObj ) || Abc_ObjIsBi( pObj ) || Abc_ObjIsBo( pObj ) )
        return;
    Abc_Print( 1, "hpart: warning: skipping unsupported object type %u (obj id %d)\n", Abc_ObjType( pObj ), Abc_ObjId( pObj ) );
    traits.warned_unknown = true;
}

void CollectSinks( Abc_Obj_t *pObj, const std::vector<int> &obj_to_vertex, std::vector<int> &sinks, std::vector<char> &visited )
{
    Abc_Obj_t *pObjR = Abc_ObjRegular( pObj );
    Abc_Obj_t *pFanout;
    int i;

    if ( pObjR == nullptr || pObjR->Id < 0 || pObjR->Id >= static_cast<int>( visited.size() ) )
        return;
    if ( visited[pObjR->Id] )
        return;
    visited[pObjR->Id] = 1;

    if ( IsHyperNode( pObjR ) )
    {
        const int vertex_id = obj_to_vertex[pObjR->Id];
        if ( vertex_id >= 0 )
            sinks.push_back( vertex_id + 1 );
        return;
    }

    if ( !ShouldTraverseInterconnect( pObjR ) )
        return;

    Abc_ObjForEachFanout( pObjR, pFanout, i )
        CollectSinks( pFanout, obj_to_vertex, sinks, visited );
}

Hypergraph BuildHypergraph( Abc_Ntk_t *pNtk, VertexTraits &traits )
{
    Hypergraph hypergraph;
    Abc_Obj_t *pObj;
    int i;
    std::vector<int> obj_to_vertex( Abc_NtkObjNumMax( pNtk ), -1 );

    hypergraph.vertices.reserve( Abc_NtkPiNum( pNtk ) + Abc_NtkPoNum( pNtk ) + Abc_NtkNodeNum( pNtk ) + Abc_NtkLatchNum( pNtk ) + 1 );
    Abc_NtkForEachObj( pNtk, pObj, i )
    {
        if ( Abc_ObjIsLatch( pObj ) )
            traits.saw_latch = true;

        if ( IsHyperNode( pObj ) )
        {
            obj_to_vertex[pObj->Id] = static_cast<int>( hypergraph.vertices.size() );
            hypergraph.vertices.push_back( pObj );
            continue;
        }

        WarnUnexpectedObject( pObj, traits );
    }

    hypergraph.edges.reserve( hypergraph.vertices.size() );
    Abc_NtkForEachObj( pNtk, pObj, i )
    {
        if ( !IsCarrierNode( pObj ) )
            continue;

        std::vector<int> pins;
        std::vector<char> visited( Abc_NtkObjNumMax( pNtk ), 0 );
        Abc_Obj_t *pCarrier = pObj;
        int carrier_vertex = obj_to_vertex[pObj->Id];

        if ( carrier_vertex < 0 )
            continue;
        pins.push_back( carrier_vertex + 1 );

        if ( Abc_ObjIsLatch( pObj ) )
        {
            if ( Abc_ObjFanoutNum( pObj ) == 0 )
                continue;
            pCarrier = Abc_ObjFanout0( pObj );
        }

        Abc_Obj_t *pFanout;
        int j;
        Abc_ObjForEachFanout( pCarrier, pFanout, j )
            CollectSinks( pFanout, obj_to_vertex, pins, visited );

        std::sort( pins.begin(), pins.end() );
        pins.erase( std::unique( pins.begin(), pins.end() ), pins.end() );
        if ( pins.size() >= 2 )
            hypergraph.edges.push_back( std::move( pins ) );
    }
    return hypergraph;
}

bool WriteHypergraph( const Hypergraph &hypergraph, const std::filesystem::path &file_name )
{
    std::ofstream out( file_name );
    if ( !out )
        return false;

    out << hypergraph.edges.size() << ' ' << hypergraph.vertices.size() << '\n';
    for ( const auto &edge : hypergraph.edges )
    {
        for ( std::size_t i = 0; i < edge.size(); ++i )
        {
            if ( i )
                out << ' ';
            out << edge[i];
        }
        out << '\n';
    }
    return true;
}

std::filesystem::path PartitionFileName( const std::filesystem::path &graph_file, int num_parts )
{
    return std::filesystem::path( graph_file.string() + PartitionFileSuffix + std::to_string( num_parts ) );
}

std::string BuildCommand( const std::filesystem::path &exe_path, const std::filesystem::path &graph_file, const Config &cfg )
{
    std::ostringstream oss;
    oss << ShellQuote( exe_path.string() ) << ' ' << ShellQuote( graph_file.string() );
    if ( cfg.tool == Tool::KMetis )
        oss << ' ' << cfg.num_parts;
    else
        oss << ' ' << cfg.num_parts << " 2 10 1 1 0 0 0";

    if ( !cfg.verbose )
        oss << " > /dev/null 2>&1";
    return oss.str();
}

bool ReadPartitionFile( const std::filesystem::path &file_name, int num_parts, std::size_t expected_vertices, std::vector<part_id> &partitions )
{
    std::ifstream in( file_name );
    if ( !in )
        return false;

    partitions.clear();
    partitions.reserve( expected_vertices );

    int part = 0;
    while ( in >> part )
    {
        if ( part < 0 || part >= num_parts )
            return false;
        partitions.push_back( static_cast<part_id>( part ) );
    }

    return partitions.size() == expected_vertices;
}

void PrintSummary( Abc_Ntk_t *pNtk, const Config &cfg )
{
    std::vector<int> counts( cfg.num_parts, 0 );
    Abc_Obj_t *pObj;
    int i;
    int cut_size = 0;
    int total_count = 0;

    Abc_NtkForEachObj( pNtk, pObj, i )
    {
        part_id part = Abc_ObjGetPartId( pObj );
        if ( Abc_PartIdIsValid( part ) && part < counts.size() )
        {
            counts[part] += 1;
            total_count += 1;
        }
        if ( Abc_ObjIsCutNet( pObj ) )
            cut_size += 1;
    }

    Abc_Print( 1, "tool = %s, parts = %d, cut size = %d\n", ToolName( cfg.tool ), cfg.num_parts, cut_size );
    for ( int part = 0; part < cfg.num_parts; ++part )
    {
        double ratio = total_count == 0 ? 0.0 : 100.0 * counts[part] / total_count;
        Abc_Print( 1, "part %d node count = %d ( %.1f%% )\n", part, counts[part], ratio );
    }
}

int CountCutSize( Abc_Ntk_t *pNtk )
{
    Abc_Obj_t *pObj;
    int i;
    int CutSize = 0;

    Abc_NtkForEachObj( pNtk, pObj, i )
        CutSize += Abc_ObjIsCutNet( pObj );
    return CutSize;
}

} // namespace

const char *ToolName( Tool tool )
{
    switch ( tool )
    {
    case Tool::HMetis:
        return "hmetis";
    case Tool::SHMetis:
        return "shmetis";
    case Tool::KMetis:
        return "kmetis";
    }
    return "unknown";
}

bool ApplyPartitioning( Abc_Ntk_t *pNtk, const Config &cfg )
{
    if ( pNtk == nullptr )
    {
        Abc_Print( -1, "hpart: current network is empty\n" );
        return false;
    }
    if ( cfg.num_parts < 2 || cfg.num_parts > ABC_PART_ID_NONE )
    {
        Abc_Print( -1, "hpart: invalid partition number %d (must be between 2 and 255)\n", cfg.num_parts );
        return false;
    }

    const std::filesystem::path exe_path = FindExecutable( ToolName( cfg.tool ) );
    if ( exe_path.empty() )
    {
        Abc_Print( -1, "hpart: partitioner \"%s\" is not available in PATH\n", ToolName( cfg.tool ) );
        return false;
    }

    VertexTraits traits;
    Hypergraph hypergraph = BuildHypergraph( pNtk, traits );
    if ( hypergraph.vertices.empty() )
    {
        Abc_Print( -1, "hpart: current network has no hypergraph vertices to partition\n" );
        return false;
    }
    if ( hypergraph.edges.empty() )
    {
        Abc_Print( -1, "hpart: current network has no hyperedges to partition\n" );
        return false;
    }
    if ( traits.saw_latch )
        Abc_Print( 1, "hpart: warning: network contains latch objects; they are included as hypergraph vertices\n" );

    TempDir temp_dir = CreateTempDir();
    if ( !temp_dir.valid )
    {
        Abc_Print( -1, "hpart: failed to create temporary directory: %s\n", std::strerror( errno ) );
        return false;
    }

    const std::filesystem::path graph_file = temp_dir.path / "network.hgr";
    const std::filesystem::path part_file = PartitionFileName( graph_file, cfg.num_parts );
    if ( !WriteHypergraph( hypergraph, graph_file ) )
    {
        Abc_Print( -1, "hpart: failed to write hypergraph file \"%s\"\n", graph_file.c_str() );
        return false;
    }

    const std::string command = BuildCommand( exe_path, graph_file, cfg );
    if ( cfg.verbose )
    {
        Abc_Print( 1, "hpart: running %s\n", command.c_str() );
        fflush( stdout );
    }

    const int status = std::system( command.c_str() );
    if ( status == -1 )
    {
        Abc_Print( -1, "hpart: failed to launch partitioner \"%s\"\n", ToolName( cfg.tool ) );
        return false;
    }
    if ( ( !WIFEXITED( status ) || WEXITSTATUS( status ) != 0 ) && !std::filesystem::exists( part_file ) )
    {
        Abc_Print( -1, "hpart: partitioner \"%s\" exited with status %d\n", ToolName( cfg.tool ), status );
        return false;
    }
    if ( ( !WIFEXITED( status ) || WEXITSTATUS( status ) != 0 ) && cfg.verbose )
        Abc_Print( 1, "hpart: partitioner returned status %d, using generated result file anyway\n", status );

    std::vector<part_id> partitions;
    if ( !ReadPartitionFile( part_file, cfg.num_parts, hypergraph.vertices.size(), partitions ) )
    {
        Abc_Print( -1, "hpart: failed to read partition file \"%s\"\n", part_file.c_str() );
        return false;
    }

    Abc_NtkClearPartIds( pNtk );
    for ( std::size_t i = 0; i < hypergraph.vertices.size(); ++i )
        Abc_ObjSetPartId( hypergraph.vertices[i], partitions[i] );
    Abc_NtkUpdateCutNets( pNtk );
    Abc_NtkSetPartStats( pNtk, cfg.num_parts, CountCutSize( pNtk ) );

    PrintSummary( pNtk, cfg );
    return true;
}

} // namespace fox::hpart

ABC_NAMESPACE_IMPL_END
