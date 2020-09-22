// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include "emitter/isel_mapping.h"



// -----------------------------------------------------------------------------
llvm::MachineFunction *ISelMapping::operator[] (const Func *func) const
{
  auto it = funcs_.find(func);
  if (it == funcs_.end()) {
    llvm::report_fatal_error("Missing function");
  }
  return it->second;
}

// -----------------------------------------------------------------------------
llvm::MCSymbol *ISelMapping::operator[] (const Inst *inst) const
{
  auto it = labels_.find(inst);
  if (it == labels_.end()) {
    llvm::report_fatal_error("Missing label");
  }
  return it->second;
}

// -----------------------------------------------------------------------------
llvm::MachineBasicBlock *ISelMapping::operator[] (const Block *block) const
{
  auto it = blocks_.find(block);
  if (it == blocks_.end()) {
    llvm::report_fatal_error("Missing block");
  }
  return it->second;
}
