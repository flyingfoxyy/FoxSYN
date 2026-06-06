#ifndef ABC__base__abc__abcPdb_hpp
#define ABC__base__abc__abcPdb_hpp

#ifdef __cplusplus

#include <algorithm>
#include <vector>

#include "abc.h"

ABC_NAMESPACE_CXX_HEADER_START

struct Pdb
{
    explicit Pdb( std::size_t nObjs = 0 )
        : m_part_ids( nObjs, ABC_PART_ID_NONE )
    {
    }

    part_id get( int ObjId ) const
    {
        if ( ObjId < 0 || static_cast<std::size_t>(ObjId) >= m_part_ids.size() )
            return ABC_PART_ID_NONE;
        return m_part_ids[ObjId];
    }

    void set( int ObjId, part_id PartId )
    {
        assert( ObjId >= 0 );
        ensure( ObjId );
        m_part_ids[ObjId] = PartId;
    }

    void clear( int ObjId )
    {
        if ( ObjId < 0 || static_cast<std::size_t>(ObjId) >= m_part_ids.size() )
            return;
        m_part_ids[ObjId] = ABC_PART_ID_NONE;
    }

    void clear_all()
    {
        std::fill( m_part_ids.begin(), m_part_ids.end(), ABC_PART_ID_NONE );
        invalidate_stats();
    }

    void set_stats( int NumParts, int CutSize, int HopNum )
    {
        m_num_parts = NumParts;
        m_cut_size = CutSize;
        m_hop_num = HopNum;
    }

    void set_balance_pct( int BalancePct )
    {
        m_balance_pct = BalancePct;
    }

    void invalidate_stats()
    {
        m_num_parts = -1;
        m_cut_size = -1;
        m_hop_num = -1;
        m_balance_pct = -1;
    }

    int num_parts() const
    {
        return m_num_parts;
    }

    int cut_size() const
    {
        return m_cut_size;
    }

    int hop_num() const
    {
        return m_hop_num;
    }

    int balance_pct() const
    {
        return m_balance_pct;
    }

private:
    void ensure( int ObjId )
    {
        if ( static_cast<std::size_t>(ObjId) >= m_part_ids.size() )
            m_part_ids.resize( static_cast<std::size_t>(ObjId) + 1, ABC_PART_ID_NONE );
    }

private:
    std::vector<part_id> m_part_ids;
    int m_num_parts = -1;
    int m_cut_size = -1;
    int m_hop_num = -1;
    int m_balance_pct = -1;
};

ABC_NAMESPACE_CXX_HEADER_END

#endif

#endif
