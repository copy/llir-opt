// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include <queue>

#include <llvm/Support/Debug.h>

#include "passes/pre_eval/reference_graph.h"
#include "passes/pre_eval/symbolic_approx.h"
#include "passes/pre_eval/symbolic_value.h"
#include "passes/pre_eval/symbolic_context.h"
#include "passes/pre_eval/symbolic_eval.h"

#define DEBUG_TYPE "pre-eval"




// -----------------------------------------------------------------------------
bool SymbolicApprox::Approximate(CallSite &call)
{
  if (auto *func = call.GetDirectCallee()) {
    if (IsAllocation(*func)) {
      if (func->getName() == "malloc") {
        if (call.arg_size() == 1 && call.type_size() == 1) {
          if (auto size = ctx_.Find(call.arg(0)).AsInt()) {
            SymbolicPointer ptr = ctx_.Malloc(call, size->getZExtValue());
            LLVM_DEBUG(llvm::dbgs() << "\t\t0: " << ptr << "\n");
            return ctx_.Set(call, SymbolicValue::Nullable(ptr));
          } else {
            SymbolicPointer ptr = ctx_.Malloc(call, std::nullopt);
            LLVM_DEBUG(llvm::dbgs() << "\t\t0: " << ptr << "\n");
            return ctx_.Set(call, SymbolicValue::Nullable(ptr));
          }
        } else {
          llvm_unreachable("not implemented");
        }
      } else if (func->getName() == "free") {
        // TODO: invalidate the object?
        return false;
      } else if (func->getName() == "realloc") {
        llvm_unreachable("not implemented");
      } else if (func->getName() == "caml_alloc_small_aux" || func->getName() == "caml_alloc_shr_aux") {
        if (call.arg_size() >= 1 && call.type_size() == 1) {
          if (auto size = ctx_.Find(call.arg(0)).AsInt()) {
            SymbolicPointer ptr = ctx_.Malloc(call, size->getZExtValue());
            LLVM_DEBUG(llvm::dbgs() << "\t\t0: " << ptr << "\n");
            return ctx_.Set(call, SymbolicValue::Nullable(ptr));
          } else {
            SymbolicPointer ptr = ctx_.Malloc(call, std::nullopt);
            LLVM_DEBUG(llvm::dbgs() << "\t\t0: " << ptr << "\n");
            return ctx_.Set(call, SymbolicValue::Nullable(ptr));
          }
        } else {
          llvm_unreachable("not implemented");
        }
      } else if (func->getName() == "caml_alloc1") {
        if (call.arg_size() == 2 && call.type_size() == 2) {
          auto ptr = SymbolicValue::Nullable(ctx_.Malloc(call, 16));
          bool changed = false;
          ctx_.Set(call.GetSubValue(0), ctx_.Find(call.arg(0))) || changed;
          ctx_.Set(call.GetSubValue(1), ptr) || changed;
          return changed;
        } else {
          llvm_unreachable("not implemented");
        }
      } else if (func->getName() == "caml_alloc2") {
        llvm_unreachable("not implemented");
      } else if (func->getName() == "caml_alloc3") {
        llvm_unreachable("not implemented");
      } else if (func->getName() == "caml_allocN") {
        llvm_unreachable("not implemented");
      } else if (func->getName() == "caml_alloc_custom_mem") {
        if (call.arg_size() == 3 && call.type_size() == 1) {
          if (auto size = ctx_.Find(call.arg(1)).AsInt()) {
            SymbolicPointer ptr = ctx_.Malloc(call, size->getZExtValue());
            return ctx_.Set(call, SymbolicValue::Nullable(ptr));
          } else {
            llvm_unreachable("not implemented");
          }
        } else {
          llvm_unreachable("not implemented");
        }
      } else {
        llvm_unreachable("not implemented");
      }
    } else {
      return Approximate(call, *func);
    }
  } else {
    llvm_unreachable("not implemented");
  }
}

// -----------------------------------------------------------------------------
bool SymbolicApprox::Approximate(CallSite &call, Func &func)
{
  SymbolicValue values = SymbolicValue::Scalar();
  for (auto arg : call.args()) {
    auto argVal = ctx_.Find(arg);
    LLVM_DEBUG(llvm::dbgs() << "\t\t\t" << argVal << "\n");
    values = values.LUB(argVal);
  }

  SymbolicPointer ptr;
  Extract(func, values, ptr);

  auto v = SymbolicValue::Pointer(ptr);
  bool changed = ptr.empty() ? false : ctx_.Store(ptr, v, Type::I64);
  for (unsigned i = 0, n = call.GetNumRets(); i < n; ++i) {
    changed = ctx_.Set(call.GetSubValue(i), v) || changed;
  }
  return changed;
}

// -----------------------------------------------------------------------------
void SymbolicApprox::Extract(
    Func &func,
    const SymbolicValue &values,
    SymbolicPointer &ptr)
{
  std::set<Func *> vf;
  std::queue<Func *> qf;
  qf.push(&func);

  std::set<Global *> globals;
  std::set<std::pair<unsigned, unsigned>> frames;
  std::set<std::pair<unsigned, CallSite *>> sites;
  Extract(values, globals, frames, sites);

  while (!qf.empty()) {
    auto &fn = *qf.front();
    qf.pop();
    if (!vf.insert(&fn).second) {
      continue;
    }

    auto &node = refs_.FindReferences(fn);
    LLVM_DEBUG(llvm::dbgs() << "\t\tCall to: " << fn.getName() << ", refs: ");
    for (auto *g : node.Referenced) {
      LLVM_DEBUG(llvm::dbgs() << g->getName() << " ");
      globals.insert(g);
    }
    ctx_.MarkExecuted(fn);
    for (auto *f : node.Called) {
      ctx_.MarkExecuted(*f);
    }

    ptr.LUB(ctx_.Taint(globals, frames, sites));
    LLVM_DEBUG(llvm::dbgs() << "\t\tTaint: " << ptr << "\n");
    if (node.HasRaise) {
      // Taint all landing pads on the stack which can be reached from here.
      // Landing pads must be tainted with incoming values in case the
      // evaluation of an invoke instruction continues with the catch block.
      for (Block *ptr : ptr.blocks()) {
        for (auto &frame : ctx_.frames()) {
          // See whether the block is among the successors of the active node
          // in any of the frames on the stack, propagating to landing pads.
          auto *exec = frame.GetCurrentNode();
          if (!exec) {
            continue;
          }

          for (auto *succ : exec->Succs) {
            for (auto *block : succ->Blocks) {
              if (block != ptr) {
                continue;
              }
              LLVM_DEBUG(llvm::dbgs() << "\t\tLanding: " << block->getName() << "\n");
              for (auto &inst : *block) {
                auto *pad = ::cast_or_null<LandingPadInst>(&inst);
                if (!pad) {
                  continue;
                }
                LLVM_DEBUG(llvm::dbgs() << "\t\t\t" << inst << "\n");
                for (unsigned i = 0, n = pad->GetNumRets(); i < n; ++i) {
                  ctx_.Set(pad->GetSubValue(i), SymbolicValue::Value(ptr));
                }
              }
            }
          }
        }
      }
    }

    if (node.HasIndirectCalls) {
      for (auto *f : ptr.funcs()) {
        LLVM_DEBUG(llvm::dbgs() << "\t\tIndirect call to: " << f->getName() << "\n");
        qf.push(f);
      }
    }
  }
}

// -----------------------------------------------------------------------------
void SymbolicApprox::Extract(
    const SymbolicValue &value,
    std::set<Global *> &pointers,
    std::set<std::pair<unsigned, unsigned>> &frames,
    std::set<std::pair<unsigned, CallSite *>> &sites)
{
  switch (value.GetKind()) {
    case SymbolicValue::Kind::SCALAR:
    case SymbolicValue::Kind::UNDEFINED:
    case SymbolicValue::Kind::INTEGER:
    case SymbolicValue::Kind::LOWER_BOUNDED_INTEGER:
    case SymbolicValue::Kind::FLOAT: {
      return;
    }
    case SymbolicValue::Kind::VALUE:
    case SymbolicValue::Kind::POINTER:
    case SymbolicValue::Kind::NULLABLE: {
      for (auto addr : value.GetPointer()) {
        switch (addr.GetKind()) {
          case SymbolicAddress::Kind::GLOBAL: {
            pointers.insert(addr.AsGlobal().Symbol);
            return;
          }
          case SymbolicAddress::Kind::GLOBAL_RANGE: {
            pointers.insert(addr.AsGlobalRange().Symbol);
            return;
          }
          case SymbolicAddress::Kind::FRAME: {
            llvm_unreachable("not implemented");
          }
          case SymbolicAddress::Kind::FRAME_RANGE: {
            llvm_unreachable("not implemented");
          }
          case SymbolicAddress::Kind::HEAP: {
            const auto &a = addr.AsHeap();
            sites.emplace(a.Frame, a.Alloc);
          }
          case SymbolicAddress::Kind::HEAP_RANGE: {
            const auto &a = addr.AsHeapRange();
            sites.emplace(a.Frame, a.Alloc);
            return;
          }
          case SymbolicAddress::Kind::FUNC: {
            llvm_unreachable("not implemented");
          }
          case SymbolicAddress::Kind::BLOCK: {
            llvm_unreachable("not implemented");
          }
          case SymbolicAddress::Kind::STACK: {
            llvm_unreachable("not implemented");
          }
        }
        llvm_unreachable("invalid address kind");
      }
      return;
    }
  }
  llvm_unreachable("invalid value kind");
}

// -----------------------------------------------------------------------------
void SymbolicApprox::Approximate(
    SymbolicFrame &frame,
    const std::set<SCCNode *> &bypassed,
    const std::set<SymbolicContext *> &contexts)
{
  // Compute the union of all contexts.
  LLVM_DEBUG(llvm::dbgs() << "Merging " << contexts.size() << " contexts\n");
  SymbolicContext merged(ctx_);
  for (auto &context : contexts) {
    merged.LUB(*context);
  }

  // If any nodes were bypassed, collect all references inside those
  // nodes, along with all additional symbols introduced in the branch.
  // Compute the transitive closure of these objects, tainting all
  // pointees with the closure as a pointer in the unified heap
  // before merging it into the current state. Map all values to this
  // tainted value, with the exception of obvious trivial constants.
  LLVM_DEBUG(llvm::dbgs() << "Collecting references\n");
  std::optional<SymbolicPointer> uses;
  std::set<CallSite *> calls;
  std::set<CallSite *> allocs;
  for (auto *node : bypassed) {
    for (Block *block : node->Blocks) {
      for (Inst &inst : *block) {
        LLVM_DEBUG(llvm::dbgs() << "\tScan " << inst << "\n");
        if (auto *call = ::cast_or_null<CallSite>(&inst)) {
          if (auto *f = call->GetDirectCallee()) {
            auto n = f->getName();
            if (n == "caml_check_urgent_gc") {

            } else if (IsAllocation(*f)) {
              allocs.insert(call);
            } else {
              calls.insert(call);
            }
          } else {
            calls.insert(call);
          }
        }
        for (Ref<Value> opValue : inst.operand_values()) {
          Ref<Inst> opInst = ::cast_or_null<Inst>(opValue);
          if (!opInst) {
            continue;
          }
          auto *usedValue = merged.FindOpt(*opInst);
          if (!usedValue) {
            continue;
          }
          if (auto ptr = usedValue->AsPointer()) {
            LLVM_DEBUG(llvm::dbgs() << "\t\t" << *ptr << "\n");
            if (uses) {
              uses->LUB(ptr->Decay());
            } else {
              uses.emplace(ptr->Decay());
            }
          }
        }
      }
    }
  }

  if (uses) {
    uses->LUB(ctx_.Taint(*uses));
  }
  if (!allocs.empty()) {
    // TODO: produce some allocations
  }
  if (!calls.empty()) {
    for (auto *site : calls) {
      if (auto *f = site->GetDirectCallee()) {
        if (uses) {
          LLVM_DEBUG(llvm::dbgs() << "Expanding " << f->getName() << "\n");
          Extract(*f, SymbolicValue::Pointer(*uses), *uses);
        } else {
          llvm_unreachable("not implemented");
        }
      } else {
        llvm_unreachable("not implemented");
      }
    }
  }

  for (auto *node : bypassed) {
    for (Block *block : node->Blocks) {
      LLVM_DEBUG(llvm::dbgs() << "\tBypass: " << block->getName() << '\n');
      for (Inst &inst : *block) {
        LLVM_DEBUG(llvm::dbgs() << "\tApprox: " << inst << '\n');
        if (auto *mov = ::cast_or_null<MovInst>(&inst)) {
          Resolve(*mov);
        } else {
          for (unsigned i = 0, n = inst.GetNumRets(); i < n; ++i) {
            auto instRef = inst.GetSubValue(i);
            if (uses) {
              ctx_.Set(instRef, SymbolicValue::Value(*uses));
            } else {
              ctx_.Set(instRef, SymbolicValue::Scalar());
            }
          }
        }
      }
    }
  }

  // Merge the expanded prior contexts into the head.
  ctx_.LUB(merged);
}

// -----------------------------------------------------------------------------
void SymbolicApprox::Resolve(MovInst &mov)
{
  // Try to register constants introduced by mov as constants
  // instead of relying on the universal over-approximated value.
  auto arg = mov.GetArg();
  switch (arg->GetKind()) {
    case Value::Kind::INST: {
      llvm_unreachable("not implemented");
    }
    case Value::Kind::GLOBAL: {
      auto &c = *::cast<Global>(arg);
      ctx_.Set(mov, SymbolicValue::Pointer(&c, 0));
      return;
    }
    case Value::Kind::EXPR: {
      llvm_unreachable("not implemented");
    }
    case Value::Kind::CONST: {
      auto &c = *::cast<Constant>(arg);
      switch (c.GetKind()) {
        case Constant::Kind::INT: {
          switch (auto ty = mov.GetType()) {
            case Type::I8:
            case Type::I16:
            case Type::I32:
            case Type::I64:
            case Type::V64:
            case Type::I128: {
              auto &ci = static_cast<ConstantInt &>(c);
              auto width = GetSize(ty) * 8;
              auto value = ci.GetValue();
              if (width != value.getBitWidth()) {
                ctx_.Set(mov, SymbolicValue::Integer(value.trunc(width)));
              } else {
                ctx_.Set(mov, SymbolicValue::Integer(value));
              }
              return;
            }
            case Type::F32:
            case Type::F64:
            case Type::F80:
            case Type::F128: {
              llvm_unreachable("not implemented");
            }
          }
          llvm_unreachable("invalid integer type");
        }
        case Constant::Kind::FLOAT: {
          llvm_unreachable("not implemented");
        }
      }
      llvm_unreachable("invalid constant kind");
    }
  }
  llvm_unreachable("invalid value kind");
}
