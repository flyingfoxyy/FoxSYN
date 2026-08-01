#ifndef PST_HPP
#define PST_HPP

#include "misc/util/abc_global.h"
#include "base/main/main.h"

namespace fox::pst {

// Partition-aware structural hashing: LUT netlist -> STRASH AIG, where every
// AND inherits its host LUT's part_id and only same-partition ANDs are shared.
bool ApplyPst(Abc_Frame_t *pAbc);

} // namespace fox::pst

#endif // PST_HPP
