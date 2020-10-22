// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#pragma once

#include "core/block.h"
#include "core/inst.h"



/**
 * Conditional jump instruction.
 *
 * Accepts a flag. If the argument is zero, the false branch is taken,
 * otherwise the true branch is taken.
 */
class JumpCondInst final : public TerminatorInst {
public:
  /// Kind of the instruction.
  static constexpr Inst::Kind kInstKind = Inst::Kind::JCC;

public:
  JumpCondInst(Ref<Inst> cond, Block *bt, Block *bf, AnnotSet &&annot);
  JumpCondInst(Ref<Inst> cond, Block *bt, Block *bf, const AnnotSet &annot);

  /// Returns the successor node.
  Block *getSuccessor(unsigned i) override;
  /// Returns the number of successors.
  unsigned getNumSuccessors() const override;

  /// Returns the condition.
  ConstRef<Inst> GetCond() const;
  /// Returns the condition.
  Ref<Inst> GetCond();

  /// Returns the true target.
  const Block *GetTrueTarget() const;
  /// Returns the true target.
  Block *GetTrueTarget();

  /// Returns the false target.
  const Block *GetFalseTarget() const;
  /// Returns the false target.
  Block *GetFalseTarget();

  /// This instruction has no side effects.
  bool HasSideEffects() const override { return false; }
  /// Instruction does not return.
  bool IsReturn() const override { return false; }
};

/**
 * Unconditional jump instruction.
 *
 * Transfers control to a basic block in the same function.
 */
class JumpInst final : public TerminatorInst {
public:
  /// Kind of the instruction.
  static constexpr Inst::Kind kInstKind = Inst::Kind::JMP;

public:
  JumpInst(Block *target, AnnotSet &&annot);
  JumpInst(Block *target, const AnnotSet &annot);

  /// Returns the successor node.
  Block *getSuccessor(unsigned i) override;
  /// Returns the number of successors.
  unsigned getNumSuccessors() const override;

  /// Returns the target.
  const Block *GetTarget() const;
  /// Returns the target.
  Block *GetTarget();

  /// This instruction has no side effects.
  bool HasSideEffects() const override { return false; }
  /// Instruction does not return.
  bool IsReturn() const override { return false; }
};

/**
 * ReturnInst
 */
class ReturnInst final : public TerminatorInst {
public:
  /// Kind of the instruction.
  static constexpr Inst::Kind kInstKind = Inst::Kind::RET;

public:
  ReturnInst(llvm::ArrayRef<Ref<Inst>> values, AnnotSet &&annot);

  /// Returns the successor node.
  Block *getSuccessor(unsigned i) override;
  /// Returns the number of successors.
  unsigned getNumSuccessors() const override;

  /// This instruction has side effects.
  bool HasSideEffects() const override { return true; }
  /// Instruction does not return.
  bool IsReturn() const override { return true; }

  /// Returns an argument at an index.
  Ref<Inst> arg(unsigned i);
  /// Returns an argument at an index.
  ConstRef<Inst> arg(unsigned i) const
  {
    return const_cast<ReturnInst *>(this)->arg(i);
  }

  /// Returns the number of arguments.
  size_t arg_size() const;
  /// Checks if the return takes any arguments.
  bool arg_empty() const;
  /// Start of the argument list.
  arg_iterator arg_begin() { return arg_iterator(this->value_op_begin()); }
  /// End of the argument list.
  arg_iterator arg_end() { return arg_iterator(this->value_op_begin() + size()); }
  /// Range of arguments.
  arg_range args() { return llvm::make_range(arg_begin(), arg_end()); }
  /// Start of the argument list.
  const_arg_iterator arg_begin() const { return const_arg_iterator(this->value_op_begin()); }
  /// End of the argument list.
  const_arg_iterator arg_end() const { return const_arg_iterator(this->value_op_begin() + size()); }
  /// Range of arguments.
  const_arg_range args() const { return llvm::make_range(arg_begin(), arg_end()); }
};

/**
 * Long jump instruction.
 *
 * Used to implement longjmp: transfers control to the program point after the
 * setjmp call. The arguments include the target basic block, the stack pointer
 * to reset to and the value to return from the setjmp call.
 */
class RaiseInst final : public TerminatorInst {
public:
  RaiseInst(
      Ref<Inst> target,
      Ref<Inst> stack,
      llvm::ArrayRef<Ref<Inst>> values,
      AnnotSet &&annot
  );

  /// Returns the successor node.
  Block *getSuccessor(unsigned i) override;
  /// Returns the number of successors.
  unsigned getNumSuccessors() const override;

  /// Returns the target.
  ConstRef<Inst> GetTarget() const;
  /// Returns the target.
  Ref<Inst> GetTarget();
  /// Returns the stack pointer.
  ConstRef<Inst> GetStack() const;
  /// Returns the stack pointer.
  Ref<Inst> GetStack();

  /// This instruction has side effects.
  bool HasSideEffects() const override { return true; }
  /// Instruction does not return.
  bool IsReturn() const override { return false; }

  /// Returns an argument at an index.
  Ref<Inst> arg(unsigned i);
  /// Returns an argument at an index.
  ConstRef<Inst> arg(unsigned i) const
  {
    return const_cast<RaiseInst *>(this)->arg(i);
  }

  /// Returns the number of arguments.
  size_t arg_size() const;
  /// Start of the argument list.
  arg_iterator arg_begin() { return arg_iterator(this->value_op_begin() + 2); }
  /// End of the argument list.
  arg_iterator arg_end() { return arg_iterator(this->value_op_begin() + size()); }
  /// Range of arguments.
  arg_range args() { return llvm::make_range(arg_begin(), arg_end()); }
  /// Start of the argument list.
  const_arg_iterator arg_begin() const { return const_arg_iterator(this->value_op_begin() + 2); }
  /// End of the argument list.
  const_arg_iterator arg_end() const { return const_arg_iterator(this->value_op_begin() + size()); }
  /// Range of arguments.
  const_arg_range args() const { return llvm::make_range(arg_begin(), arg_end()); }
};


/**
 * Switch instruction
 *
 * Lowers to an efficient jump table. Takes a control index argument,
 * along with a table of successor blocks. If the control index is out of
 * bounds, behaviour is undefined.
 */
class SwitchInst final : public TerminatorInst {
public:
  /// Kind of the instruction.
  static constexpr Inst::Kind kInstKind = Inst::Kind::SWITCH;

public:
  /// Constructs a switch instruction.
  SwitchInst(
      Ref<Inst> index,
      llvm::ArrayRef<Block *> branches,
      AnnotSet &&annot
  );
  /// Constructs a switch instruction.
  SwitchInst(
      Ref<Inst> index,
      llvm::ArrayRef<Block *> branches,
      const AnnotSet &annot
  );

  /// Returns the number of successors.
  unsigned getNumSuccessors() const override;
  /// Returns the successor node.
  Block *getSuccessor(unsigned i) override;
  /// Returns a successor.
  inline const Block *getSuccessor(unsigned idx) const
  {
    return const_cast<SwitchInst *>(this)->getSuccessor(idx);
  }

  /// Returns the index value.
  ConstRef<Inst> GetIdx() const;
  /// Returns the index value.
  Ref<Inst> GetIdx();

  /// This instruction has no side effects.
  bool HasSideEffects() const override { return false; }
  /// Instruction does not return.
  bool IsReturn() const override { return false; }
};

/**
 * Trap instruction which terminates a block.
 *
 * The trap instruction should never be reached by execution. It lowers to
 * an illegal instruction to aid debugging.
 */
class TrapInst final : public TerminatorInst {
public:
  /// Constructs a trap instruction.
  TrapInst(AnnotSet &&annot);
  /// Constructs a trap instruction.
  TrapInst(const AnnotSet &annot);

  /// Returns the successor node.
  Block *getSuccessor(unsigned i) override;
  /// Returns the number of successors.
  unsigned getNumSuccessors() const override;

  /// This instruction has side effects.
  bool HasSideEffects() const override { return true; }
  /// Instruction does not return.
  bool IsReturn() const override { return false; }
};
