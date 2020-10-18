// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include <sstream>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/SmallPtrSet.h>

#include "core/block.h"
#include "core/cast.h"
#include "core/cfg.h"
#include "core/func.h"
#include "core/prog.h"
#include "core/insts.h"
#include "passes/verifier.h"



// -----------------------------------------------------------------------------
const char *VerifierPass::kPassID = "verifier";

// -----------------------------------------------------------------------------
void VerifierPass::Run(Prog *prog)
{
  for (Func &func : *prog) {
    Verify(func);
  }
}

// -----------------------------------------------------------------------------
const char *VerifierPass::GetPassName() const
{
  return "Verifier";
}

// -----------------------------------------------------------------------------
void VerifierPass::Verify(Func &func)
{
  for (Block &block : func) {
    for (Inst &inst : block) {
      Verify(inst);
    }
  }
}

// -----------------------------------------------------------------------------
static bool Compatible(Type vt, Type type)
{
  if (vt == type) {
    return true;
  }
  if (type == Type::V64 || type == Type::I64) {
    return vt == Type::I64 || vt == Type::V64;
  }
  return false;
}

// -----------------------------------------------------------------------------
void VerifierPass::Verify(Inst &i)
{
  auto GetType = [&, this](Inst *inst) {
    if (inst->GetNumRets() == 0) {
      Error(i, "missing type");
    }
    return inst->GetType(0);
  };
  auto CheckType = [&, this](Inst *inst, Type type) {
    auto it = GetType(inst);
    if (type == Type::I64 && it == Type::V64) {
      return;
    }
    if (type == Type::V64 && it == Type::I64) {
      return;
    }
    if (it != type) {
      Error(i, "invalid type");
    }
  };
  auto CheckPointer = [&, this](Inst *inst, const char *msg = "not a pointer") {
    auto type = GetType(inst);
    if (type != Type::I64 && type != Type::V64) {
      Error(i, msg);
    }
  };
  auto CheckInteger = [&, this](Inst *inst) {
    auto type = GetType(inst);
    if (!IsIntegerType(type)) {
      Error(i, "not a pointer");
    }
  };

  switch (i.GetKind()) {
    case Inst::Kind::CALL:
    case Inst::Kind::TCALL:
    case Inst::Kind::INVOKE: {
      auto &call = static_cast<CallSite &>(i);
      CheckPointer(call.GetCallee());
      // TODO: check arguments for direct callees.
      return;
    }
    case Inst::Kind::SYSCALL: {
      return;
    }
    case Inst::Kind::CLONE: {
      return;
    }

    case Inst::Kind::ARG: {
      auto &arg = static_cast<ArgInst &>(i);
      unsigned idx = arg.GetIdx();
      const auto &params = i.getParent()->getParent()->params();
      if (idx >= params.size()) {
        Error(i, "argument out of range");
      }
      if (params[idx] != arg.GetType()) {
        Error(i, "argument type mismatch");
      }
      return;
    }

    case Inst::Kind::RAISE: {
      auto &inst = static_cast<RaiseInst &>(i);
      CheckPointer(inst.GetTarget());
      CheckPointer(inst.GetStack());
      return;
    }
    case Inst::Kind::LD: {
      CheckPointer(static_cast<LoadInst &>(i).GetAddr());
      return;
    }
    case Inst::Kind::ST: {
      CheckPointer(static_cast<StoreInst &>(i).GetAddr());
      return;
    }
    case Inst::Kind::X86_FNSTCW:
    case Inst::Kind::X86_FNSTSW:
    case Inst::Kind::X86_FNSTENV:
    case Inst::Kind::X86_FLDCW:
    case Inst::Kind::X86_FLDENV:
    case Inst::Kind::X86_LDMXCSR:
    case Inst::Kind::X86_STMXCSR: {
      auto &inst = static_cast<X86_FPUControlInst &>(i);
      CheckPointer(inst.GetAddr());
      return;
    }
    case Inst::Kind::VASTART:{
      CheckPointer(static_cast<VAStartInst &>(i).GetVAList());
      return;
    }

    case Inst::Kind::X86_XCHG: {
      auto &xchg = static_cast<X86_XchgInst &>(i);
      CheckPointer(xchg.GetAddr());
      if (GetType(xchg.GetVal()) != xchg.GetType()) {
        Error(i, "invalid exchange");
      }
      return;
    }
    case Inst::Kind::X86_CMPXCHG: {
      auto &cmpXchg = static_cast<X86_CmpXchgInst &>(i);
      CheckPointer(cmpXchg.GetAddr());
      if (GetType(cmpXchg.GetVal()) != cmpXchg.GetType()) {
        Error(i, "invalid exchange");
      }
      if (GetType(cmpXchg.GetRef()) != cmpXchg.GetType()) {
        Error(i, "invalid exchange");
      }
      return;
    }
    case Inst::Kind::SELECT: {
      auto &sel = static_cast<SelectInst &>(i);
      if (!Compatible(GetType(sel.GetTrue()), sel.GetType())) {
        Error(i, "mismatched true branch");
      }
      if (!Compatible(GetType(sel.GetFalse()), sel.GetType())) {
        Error(i, "mismatched false branches");
      }
      return;
    }

    case Inst::Kind::SEXT:
    case Inst::Kind::ZEXT:
    case Inst::Kind::FEXT:
    case Inst::Kind::XEXT:
    case Inst::Kind::TRUNC: {
      return;
    }

    case Inst::Kind::PHI: {
      auto &phi = static_cast<PhiInst &>(i);
      Type type = phi.GetType();
      for (unsigned i = 0, n = phi.GetNumIncoming(); i < n; ++i) {
        Value *value = phi.GetValue(i);
        switch (value->GetKind()) {
          case Value::Kind::INST: {
            auto vt = GetType(static_cast<Inst *>(value));
            if (!Compatible(vt, type)) {
              Error(phi, "phi instruction argument invalid");
            }
            continue;
          }
          case Value::Kind::GLOBAL: {
            CheckPointer(&phi, "phi must be of pointer type");
            continue;
          }
          case Value::Kind::EXPR: {
            switch (static_cast<Expr *>(value)->GetKind()) {
              case Expr::Kind::SYMBOL_OFFSET: {
                CheckPointer(&phi, "phi must be of pointer type");
                continue;
              }
            }
            llvm_unreachable("invalid expression kind");
          }
          case Value::Kind::CONST: {
            switch (static_cast<Constant *>(value)->GetKind()) {
              case Constant::Kind::INT: {
                continue;
              }
              case Constant::Kind::FLOAT: {
                return;
              }
              case Constant::Kind::REG: {
                llvm_unreachable("invalid incoming register to phi");
              }
            }
            llvm_unreachable("invalid constant kind");
          }
        }
        llvm_unreachable("invalid value kind");
      }
      return;
    }

    case Inst::Kind::SET: {
      auto &set = static_cast<SetInst &>(i);
      switch (set.GetReg()->GetValue()) {
        case ConstantReg::Kind::SP:
        case ConstantReg::Kind::FS:
        case ConstantReg::Kind::RET_ADDR:
        case ConstantReg::Kind::FRAME_ADDR:
        case ConstantReg::Kind::PC: {
          CheckPointer(set.GetValue(), "set expects a pointer");
          return;
        }
      }
      llvm_unreachable("invalid register kind");
    }

    case Inst::Kind::ALLOCA:
    case Inst::Kind::FRAME: {
      if (GetType(&i) != GetPointerType()) {
        Error(i, "pointer type expected");
      }
      return;
    }

    case Inst::Kind::MOV: {
      auto &mi = static_cast<MovInst &>(i);
      Value *value = mi.GetArg();
      switch (value->GetKind()) {
        case Value::Kind::INST: {
          return;
        }
        case Value::Kind::GLOBAL: {
          CheckPointer(&mi, "global move not pointer sized");
          return;
        }
        case Value::Kind::EXPR: {
          switch (static_cast<Expr *>(value)->GetKind()) {
            case Expr::Kind::SYMBOL_OFFSET: {
              CheckPointer(&mi, "expression must be a pointer");
              return;
            }
          }
          llvm_unreachable("invalid expression kind");
        }
        case Value::Kind::CONST: {
          switch (static_cast<Constant *>(value)->GetKind()) {
            case Constant::Kind::INT: {
              return;
            }
            case Constant::Kind::FLOAT: {
              return;
            }
            case Constant::Kind::REG: {
              auto *reg = static_cast<ConstantReg *>(value);
              switch (reg->GetValue()) {
                case ConstantReg::Kind::SP:
                case ConstantReg::Kind::FS:
                case ConstantReg::Kind::RET_ADDR:
                case ConstantReg::Kind::FRAME_ADDR:
                case ConstantReg::Kind::PC: {
                  CheckPointer(&mi, "registers return pointers");
                  return;
                }
              }
              llvm_unreachable("invalid register kind");
            }
          }
          llvm_unreachable("invalid constant kind");
        }
      }
      llvm_unreachable("invalid value kind");
    }

    case Inst::Kind::X86_RDTSC:
    case Inst::Kind::X86_FNCLEX:
    case Inst::Kind::UNDEF:
    case Inst::Kind::SWITCH:
    case Inst::Kind::JCC:
    case Inst::Kind::JMP:
    case Inst::Kind::TRAP:
    case Inst::Kind::RET: {
      return;
    }

    case Inst::Kind::ABS:
    case Inst::Kind::NEG:
    case Inst::Kind::SQRT:
    case Inst::Kind::SIN:
    case Inst::Kind::COS:
    case Inst::Kind::EXP:
    case Inst::Kind::EXP2:
    case Inst::Kind::LOG:
    case Inst::Kind::LOG2:
    case Inst::Kind::LOG10:
    case Inst::Kind::FCEIL:
    case Inst::Kind::FFLOOR:
    case Inst::Kind::POPCNT:
    case Inst::Kind::CLZ:
    case Inst::Kind::CTZ: {
      // Argument must match type.
      auto &ui = static_cast<UnaryInst &>(i);
      Type type = ui.GetType();

      Inst *lhs = ui.GetArg();
      if (lhs->GetNumRets() == 0) {
        Error(i, "missing argument type");
      }
      if (lhs->GetType(0) != type) {
        Error(i, "invalid argument type");
      }
      return;
    }

    case Inst::Kind::ADD:
    case Inst::Kind::SUB:
    case Inst::Kind::AND:
    case Inst::Kind::OR:
    case Inst::Kind::XOR: {
      // TODO: check v64 operations.
      return;
    }
    case Inst::Kind::UDIV:
    case Inst::Kind::SDIV:
    case Inst::Kind::UREM:
    case Inst::Kind::SREM:
    case Inst::Kind::MUL:
    case Inst::Kind::POW:
    case Inst::Kind::COPYSIGN: {
      // All types must match.
      auto &bi = static_cast<BinaryInst &>(i);
      Type type = bi.GetType();
      CheckType(bi.GetLHS(), type);
      CheckType(bi.GetRHS(), type);
      return;
    }

    case Inst::Kind::CMP: {
      // Arguments must be of identical type.
      auto &bi = static_cast<CmpInst &>(i);
      auto lt = GetType(bi.GetLHS());
      auto rt = GetType(bi.GetRHS());
      bool lptr = lt == Type::V64 && rt == Type::I64;
      bool rptr = rt == Type::V64 && lt == Type::I64;
      if (lt != rt && !lptr && !rptr) {
        Error(i, "invalid arguments to comparison");
      }
      return;
    }


    case Inst::Kind::ROTL:
    case Inst::Kind::ROTR:
    case Inst::Kind::SLL:
    case Inst::Kind::SRA:
    case Inst::Kind::SRL: {
      // LHS must be integral and match, rhs also integral.
      auto &bi = static_cast<BinaryInst &>(i);
      Type type = bi.GetType();
      if (!IsIntegerType(type)) {
        Error(i, "integral type expected");
      }
      CheckType(bi.GetLHS(), type);
      if (!IsIntegerType(GetType(bi.GetRHS()))) {
        Error(i, "integral type expected");
      }
      return;
    }

    case Inst::Kind::UADDO:
    case Inst::Kind::UMULO:
    case Inst::Kind::USUBO:
    case Inst::Kind::SADDO:
    case Inst::Kind::SMULO:
    case Inst::Kind::SSUBO: {
      // LHS must match RHS, return type integral.
      auto &bi = static_cast<OverflowInst &>(i);
      Type type = bi.GetType();
      if (!IsIntegerType(type)) {
        Error(i, "integral type expected");
      }
      if (GetType(bi.GetLHS()) != GetType(bi.GetRHS())) {
        Error(i, "invalid argument types");
      }
      return;
    }
  }
  llvm_unreachable("invalid instruction kind");
}

// -----------------------------------------------------------------------------
void VerifierPass::Error(Inst &i, const char *msg)
{
  const Block *block = i.getParent();
  const Func *func = block->getParent();
  std::ostringstream os;
  os << "[" << func->GetName() << ":" << block->GetName() << "] " << msg;
  llvm::report_fatal_error(os.str().c_str());
}
