// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#pragma once

#include <vector>
#include <optional>

#include <llvm/ADT/ilist_node.h>
#include <llvm/ADT/ilist.h>

#include "core/annot.h"
#include "core/constant.h"
#include "core/cond.h"
#include "core/expr.h"
#include "core/type.h"
#include "core/value.h"

class Block;
class Inst;
class Context;
class Symbol;



/**
 * Traits to handle parent links from instructions.
 */
template <> struct llvm::ilist_traits<Inst> {
private:
  using instr_iterator = simple_ilist<Inst>::iterator;

public:
  void deleteNode(Inst *inst);
  void addNodeToList(Inst *inst);
  void removeNodeFromList(Inst *inst);
  void transferNodesFromList(
      ilist_traits &from,
      instr_iterator first,
      instr_iterator last
  );

  Block *getParent();
};

/**
 * Basic instruction.
 */
class Inst
  : public llvm::ilist_node_with_parent<Inst, Block>
  , public User
{
public:
  /// Kind of the instruction.
  static constexpr Value::Kind kValueKind = Value::Kind::INST;

public:
  template<typename It, typename Jt, typename U>
  using adapter = llvm::iterator_adaptor_base
      < It
      , Jt
      , std::random_access_iterator_tag
      , U
      , ptrdiff_t
      , U
      , U
      >;

  class arg_iterator
    : public adapter
        < arg_iterator
        , User::value_op_iterator
        , Ref<Inst>
        >
  {
  public:
    explicit arg_iterator(User::value_op_iterator it)
      : adapter
          < arg_iterator
          , User::value_op_iterator
          , Ref<Inst>
          >(it)
    {
    }

    Ref<Inst> operator*() const;
    Ref<Inst> operator->() const;
  };

  class const_arg_iterator
    : public adapter
        < const_arg_iterator
        , User::const_value_op_iterator
        , ConstRef<Inst>
        >
  {
  public:
    explicit const_arg_iterator(User::const_value_op_iterator it)
      : adapter
          < const_arg_iterator
          , User::const_value_op_iterator
          , ConstRef<Inst>
          >(it)
    {
    }

    ConstRef<Inst> operator*() const;
    ConstRef<Inst> operator->() const;
  };

  using arg_range = llvm::iterator_range<arg_iterator>;
  using const_arg_range = llvm::iterator_range<const_arg_iterator>;

public:
  /**
   * Enumeration of instruction types.
   */
  enum class Kind : uint8_t {
    // Control flow.
    CALL, TCALL, INVOKE, RET,
    JCC, JMP, SWITCH, TRAP,
    RAISE,
    // Memory.
    LD, ST,
    // Variable argument lists.
    VASTART,
    // Dynamic stack allcoation.
    ALLOCA,
    // Constants.
    ARG, FRAME, UNDEF,
    // Conditional.
    SELECT,
    // Unary instructions.
    ABS, NEG, SQRT, SIN, COS,
    SEXT, ZEXT, FEXT, XEXT,
    MOV, TRUNC,
    EXP, EXP2, LOG, LOG2, LOG10,
    FCEIL, FFLOOR,
    POPCNT,
    CLZ, CTZ,
    BSWAP,
    // Binary instructions.
    ADD, AND, CMP,
    UDIV, UREM,
    SDIV, SREM,
    MUL, OR,
    ROTL, ROTR,
    SLL, SRA, SRL, SUB, XOR,
    POW, COPYSIGN,
    // Overflow tests.
    UADDO, UMULO, USUBO,
    SADDO, SMULO, SSUBO,
    // PHI node.
    PHI,
    // Generic hardware instructions.
    SET,
    SYSCALL,
    CLONE,
    // X86_64 Hardware Instructions.
    X86_XCHG,
    X86_CMPXCHG,
    X86_FNSTCW,
    X86_FNSTSW,
    X86_FNSTENV,
    X86_FLDCW,
    X86_FLDENV,
    X86_LDMXCSR,
    X86_STMXCSR,
    X86_FNCLEX,
    X86_RDTSC,
    // AArch64 Hardware Instructions.
    AARCH64_LL,
    AARCH64_SC,
    AARCH64_DMB,
    // RISC-V Hardware Instructions.
    RISCV_CMPXCHG,
    RISCV_FENCE,
  };

  /// Destroys an instruction.
  virtual ~Inst();

  /// Removes an instruction from the parent.
  void removeFromParent();
  /// Removes an instruction from the parent and deletes it.
  void eraseFromParent();

  /// Returns the instruction kind.
  Kind GetKind() const { return kind_; }
  /// Checks if the instruction is of a specific kind.
  bool Is(Kind kind) const { return GetKind() == kind; }
  /// Returns the parent node.
  Block *getParent() const { return parent_; }
  /// Returns the number of returned values.
  virtual unsigned GetNumRets() const = 0;
  /// Returns the type of the ith return value.
  virtual Type GetType(unsigned i) const = 0;

  /// Checks if the instruction is void.
  bool IsVoid() const { return GetNumRets() == 0; }

  /// Checks if the instruction returns from the function.
  virtual bool IsReturn() const = 0;
  /// Checks if the instruction is constant.
  virtual bool IsConstant() const = 0;

  /// Returns the size of the instruction.
  virtual std::optional<size_t> GetSize() const { return std::nullopt; }

  /// Checks if the instruction is a terminator.
  virtual bool IsTerminator() const { return false; }

  /// Checks if a flag is set.
  template<typename T>
  bool HasAnnot() const { return annot_.Has<T>(); }

  /// Removes an annotation.
  template<typename T>
  bool ClearAnnot() { return annot_.Clear<T>(); }

  /// Returns an annotation.
  template<typename T>
  const T *GetAnnot() const { return annot_.Get<T>(); }

  /// Sets an annotation.
  template<typename T, typename... Args>
  bool SetAnnot(Args&&... args)
  {
    return annot_.Set<T, Args...>(std::forward<Args>(args)...);
  }

  /// Adds an annotation.
  bool AddAnnot(const Annot &annot) { return annot_.Add(annot); }

  /// Returns the instruction's annotation.
  const AnnotSet &GetAnnots() const { return annot_; }

  /// Returns the number of annotations.
  size_t annot_size() const { return annot_.size(); }
  /// Checks if any flags are set.
  bool annot_empty() const { return annot_.empty(); }
  /// Iterator over annotations.
  llvm::iterator_range<AnnotSet::const_iterator> annots() const
  {
    return llvm::make_range(annot_.begin(), annot_.end());
  }

  /// Checks if the instruction has side effects.
  virtual bool HasSideEffects() const = 0;

  /// Returns a unique, stable identifier for the instruction.
  unsigned GetOrder() const { return order_; }

  /// Returns the ith sub-value.
  Ref<Inst> GetSubValue(unsigned i) { return Ref(this, i); }
  /// Returns the ith sub-value.
  ConstRef<Inst> GetSubValue(unsigned i) const { return ConstRef(this, i); }

  /// Replaces all uses of this value.
  void replaceAllUsesWith(Value *v) override;
  /// Replaces all uses of a multi-type value.
  void replaceAllUsesWith(llvm::ArrayRef<Ref<Inst>> v);

  /// Replaces all uses of a multi-type value.
  template <typename T>
  typename std::enable_if<std::is_base_of<Inst, T>::value>::type
  replaceAllUsesWith(llvm::ArrayRef<Ref<T>> insts)
  {
    std::vector<Ref<Inst>> values;
    for (Ref<T> inst : insts) {
      values.push_back(inst);
    }
    return replaceAllUsesWith(values);
  }

protected:
  /// Constructs an instruction of a given type.
  Inst(Kind kind, unsigned numOps, AnnotSet &&annot);
  /// Constructs an instruction of a given type.
  Inst(Kind kind, unsigned numOps, const AnnotSet &annot);

private:
  friend struct llvm::ilist_traits<Inst>;
  /// Updates the parent node.
  void setParent(Block *parent) { parent_ = parent; }

private:
  /// Instruction kind.
  const Kind kind_;
  /// Instruction annotation.
  AnnotSet annot_;

protected:
  /// Parent node.
  Block *parent_;
  /// Unique number for stable ordering.
  unsigned order_;
};

/**
 * Base class for instructions that are involved in control flow.
 */
class ControlInst : public Inst {
public:
  /// Constructs a control flow instructions.
  ControlInst(Kind kind, unsigned numOps, AnnotSet &&annot)
    : Inst(kind, numOps, std::move(annot))
  {
  }
  /// Constructs a control flow instructions.
  ControlInst(Kind kind, unsigned numOps, const AnnotSet &annot)
    : Inst(kind, numOps, annot)
  {
  }
};

/**
 * Base class for basic block terminators.
 */
class TerminatorInst : public ControlInst {
public:
  /// Constructs a terminator instruction.
  TerminatorInst(Kind kind, unsigned numOps, AnnotSet &&annot)
    : ControlInst(kind, numOps, std::move(annot))
  {
  }

  /// Constructs a terminator instruction.
  TerminatorInst(Kind kind, unsigned numOps, const AnnotSet &annot)
    : ControlInst(kind, numOps, annot)
  {
  }

  /// Instruction is not constant.
  bool IsConstant() const override { return false; }
  /// Checks if the instruction is a terminator.
  bool IsTerminator() const override { return true; }

  /// Terminators do not return values.
  virtual unsigned GetNumRets() const override;
  /// Returns the type of the ith return value.
  Type GetType(unsigned i) const override;

  /// Returns the number of successors.
  virtual unsigned getNumSuccessors() const = 0;
  /// Returns a successor.
  virtual Block *getSuccessor(unsigned idx) = 0;
  /// Returns a successor.
  inline const Block *getSuccessor(unsigned idx) const
  {
    return const_cast<TerminatorInst *>(this)->getSuccessor(idx);
  }
};

class MemoryInst : public Inst {
public:
  /// Constructs a memory instruction.
  MemoryInst(Kind kind, unsigned numOps, AnnotSet &&annot)
    : Inst(kind, numOps, std::move(annot))
  {
  }

  /// Instruction is not constant.
  bool IsConstant() const override { return false; }
  /// Instruction does not return.
  bool IsReturn() const override { return false; }
};

class StackInst : public MemoryInst {
public:
  /// Constructs a stack instruction.
  StackInst(Kind kind, unsigned numOps, AnnotSet &&annot)
    : MemoryInst(kind, numOps, std::move(annot))
  {
  }
};

/**
 * Instruction with a single typed return.
 */
class OperatorInst : public Inst {
public:
  /// Constructs an instruction.
  OperatorInst(Kind kind, Type type, unsigned numOps, AnnotSet &&annot)
    : Inst(kind, numOps, std::move(annot))
    , type_(type)
  {
  }

  /// Constructs an instruction.
  OperatorInst(Kind kind, Type type, unsigned numOps, const AnnotSet &annot)
    : Inst(kind, numOps, annot)
    , type_(type)
  {
  }

  /// Unary operators return a single value.
  unsigned GetNumRets() const override;
  /// Returns the type of the ith return value.
  Type GetType(unsigned i) const override;

  /// Returns the type of the instruction.
  Type GetType() const { return type_; }

  /// These instructions have no side effects.
  bool HasSideEffects() const override { return false; }
  /// Instruction does not return.
  bool IsReturn() const override { return false; }

private:
  /// Return value type.
  Type type_;
};

/**
 * Instruction with a constant operand.
 */
class ConstInst : public OperatorInst {
public:
  /// Constructs a constant instruction.
  ConstInst(Kind kind, Type type, unsigned numOps, AnnotSet &&annot)
    : OperatorInst(kind, type, numOps, std::move(annot))
  {
  }
  /// Constructs a constant instruction.
  ConstInst(Kind kind, Type type, unsigned numOps, const AnnotSet &annot)
    : OperatorInst(kind, type, numOps, annot)
  {
  }

  /// Instruction is constant.
  bool IsConstant() const override { return true; }
};

/*
 * Instruction with a unary operand.
 */
class UnaryInst : public OperatorInst {
public:
  /// Constructs a unary operator instruction.
  UnaryInst(Kind kind, Type type, Ref<Inst> arg, AnnotSet &&annot);

  /// Returns the sole argument.
  ConstRef<Inst> GetArg() const ;
  /// Returns the sole argument.
  Ref<Inst> GetArg();

  /// Instruction is not constant.
  bool IsConstant() const override { return false; }
};

/**
 * Instructions with two operands.
 */
class BinaryInst : public OperatorInst {
public:
  /// Constructs a binary operator instruction.
  BinaryInst(Kind kind, Type type, Ref<Inst> lhs, Ref<Inst> rhs, AnnotSet &&annot);

  /// Returns the LHS operator.
  ConstRef<Inst> GetLHS() const;
  /// Returns the LHS operator.
  Ref<Inst> GetLHS();
  /// Returns the RHS operator.
  ConstRef<Inst> GetRHS() const;
  /// Returns the RHS operator.
  Ref<Inst> GetRHS();

  /// Instruction is not constant.
  bool IsConstant() const override { return false; }
};

/**
 * Overflow-checking instructions.
 */
class OverflowInst : public BinaryInst {
public:
  /// Constructs an overflow-checking instruction.
  OverflowInst(Kind kind, Type type, Ref<Inst> lhs, Ref<Inst> rhs, AnnotSet &&annot)
    : BinaryInst(kind, type, lhs, rhs, std::move(annot))
  {
  }

  /// Instruction is not constant.
  bool IsConstant() const override { return false; }
};
