// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#pragma once

#include <llvm/Support/raw_ostream.h>

#include "core/block.h"

class Prog;
class Func;



/**
 * Coq IR emitter
 */
class CoqEmitter final {
public:
  /// Creates a coq emitter.
  CoqEmitter(llvm::raw_ostream &os);

  /// Writes a program.
  void Write(const Prog &prog);

private:
  /// Writes a function.
  void Write(const Func &func);
  /// Writes an instruction.
  void Write(Block::const_iterator it);
  /// Writes a unary instruction.
  void Unary(Block::const_iterator it, const char *op);
  /// Writes a binary instruction.
  void Binary(Block::const_iterator it, const char *op);
  /// Writes a mov instruction.
  void Mov(Block::const_iterator it);

private:
  /// Mapping from instructions to IDs.
  std::unordered_map<const Inst *, unsigned> insts_;
  /// Mapping from blocks to IDs.
  std::unordered_map<const Block *, unsigned> blocks_;
  /// Stream to write to.
  llvm::raw_ostream &os_;
};
