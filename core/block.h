// This file if part of the genm-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#pragma once

#include <vector>
#include "adt/chain.h"
#include "core/block.h"
#include "core/inst.h"

class Func;



/**
 * Basic block.
 */
class Block : ChainNode<Block> {
public:
  Block();

  void AddInst(Inst *inst);

private:
  /// Parent function.
  Func *func_;
  /// First instruction.
  Inst *fst_;
  /// Last instruction.
  Inst *lst_;
};
