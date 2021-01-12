// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include <queue>

#include "core/extern.h"
#include "core/atom.h"

#include <llvm/Support/Debug.h>

#include "core/analysis/reference_graph.h"
#include "passes/pre_eval/heap_graph.h"
#include "passes/pre_eval/symbolic_approx.h"
#include "passes/pre_eval/symbolic_value.h"
#include "passes/pre_eval/symbolic_context.h"
#include "passes/pre_eval/symbolic_eval.h"

#define DEBUG_TYPE "pre-eval"



// -----------------------------------------------------------------------------
bool IsAllocation(Func &func)
{
  auto name = func.getName();
  return name == "malloc"
      || name == "free"
      || name == "realloc"
      || name == "caml_alloc_shr"
      || name == "caml_alloc_shr_aux"
      || name == "caml_alloc_small_aux"
      || name == "caml_alloc1"
      || name == "caml_alloc2"
      || name == "caml_alloc3"
      || name == "caml_allocN"
      || name == "caml_alloc_custom_mem"
      || name == "caml_gc_dispatch";
}

// -----------------------------------------------------------------------------
bool SymbolicApprox::Approximate(CallSite &call)
{
  if (auto *func = call.GetDirectCallee()) {
    if (IsAllocation(*func)) {
      LLVM_DEBUG(llvm::dbgs() << "Allocation " << func->getName() << "\n");
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
      } else if (func->getName() == "caml_gc_dispatch") {
        return false;
      } else {
        llvm_unreachable("not implemented");
      }
    } else {
      return ApproximateCall(call);
    }
  } else {
    return ApproximateCall(call);
  }
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

  auto addOperand = [&](Ref<Value> opValue)
  {
    Ref<Inst> opInst = ::cast_or_null<Inst>(opValue);
    if (!opInst) {
      return;
    }
    auto *usedValue = merged.FindOpt(*opInst);
    if (!usedValue) {
      return;
    }
    if (auto ptr = usedValue->AsPointer()) {
      LLVM_DEBUG(llvm::dbgs() << "\t\t" << *ptr << "\n");
      if (uses) {
        uses->LUB(ptr->Decay());
      } else {
        uses.emplace(ptr->Decay());
      }
    }
  };

  for (auto *node : bypassed) {
    for (Block *block : node->Blocks) {
      for (Inst &inst : *block) {
        LLVM_DEBUG(llvm::dbgs() << "\tScan " << inst << "\n");
        if (auto *call = ::cast_or_null<CallSite>(&inst)) {
          if (auto *f = call->GetDirectCallee()) {
            auto n = f->getName();
            if (IsAllocation(*f)) {
              allocs.insert(call);
            } else {
              calls.insert(call);
            }
          } else {
            calls.insert(call);
          }
          for (auto op : call->args()) {
            addOperand(op);
          }
        } else {
          for (Ref<Value> op : inst.operand_values()) {
            addOperand(op);
          }
        }
      }
    }
  }

  auto value = uses ? SymbolicValue::Value(*uses) : SymbolicValue::Scalar();
  auto [changed, raises] = ApproximateNodes(calls, allocs, value, merged);

  // Merge the expanded prior contexts into the head.
  ctx_.LUB(merged);

  // Set the values defined in the blocks.
  for (auto *node : bypassed) {
    for (Block *block : node->Blocks) {
      LLVM_DEBUG(llvm::dbgs() << "\tBypass: " << block->getName() << '\n');
      for (Inst &inst : *block) {
        LLVM_DEBUG(llvm::dbgs() << "\tApprox: " << inst << '\n');
        if (auto *mov = ::cast_or_null<MovInst>(&inst)) {
          Resolve(*mov);
        } else {
          for (unsigned i = 0, n = inst.GetNumRets(); i < n; ++i) {
            ctx_.Set(inst.GetSubValue(i), value);
          }
        }
      }
    }
  }

  // Raise, if necessary.
  if (raises) {
    Raise(value);
  }
}

// -----------------------------------------------------------------------------
bool SymbolicApprox::ApproximateCall(CallSite &call)
{
  SymbolicValue value = SymbolicValue::Scalar();
  for (auto arg : call.args()) {
    auto argVal = ctx_.Find(arg);
    LLVM_DEBUG(llvm::dbgs() << "\t\t\t" << argVal << "\n");
    value = value.LUB(argVal);
  }
  auto [changed, raises] = ApproximateNodes({ &call }, {}, value, ctx_);
  for (unsigned i = 0, n = call.GetNumRets(); i < n; ++i) {
    changed = ctx_.Set(call.GetSubValue(i), value) || changed;
  }
  // Raise, if necessary.
  if (raises) {
    changed = Raise(value) || changed;
  }
  return changed;
}


// -----------------------------------------------------------------------------
std::pair<bool, bool> SymbolicApprox::ApproximateNodes(
    const std::set<CallSite *> &calls,
    const std::set<CallSite *> &allocs,
    SymbolicValue &refs,
    SymbolicContext &ctx)
{
  HeapGraph heap(ctx);
  std::set<Func *> funcs;
  BitSet<HeapGraph::Node> nodes;
  bool indirect = false;
  bool raises = false;

  // Find items referenced from the values.
  heap.Extract(refs, funcs, nodes);

  // Find items referenced by the calls.
  for (auto *call : calls) {
    if (auto *f = call->GetDirectCallee()) {
      LLVM_DEBUG(llvm::dbgs() << "Direct call: " << f->getName() << "\n");
      auto &node = refs_[*f];
      indirect = indirect || node.HasIndirectCalls;
      raises = raises || node.HasRaise;
      for (auto *g : node.Referenced) {
        LLVM_DEBUG(llvm::dbgs() << "\t" << g->getName() << "\n");
        switch (g->GetKind()) {
          case Global::Kind::FUNC: {
            funcs.insert(static_cast<Func *>(g));
            continue;
          }
          case Global::Kind::ATOM: {
            auto *obj = static_cast<Atom *>(g)->getParent();
            heap.Extract(obj, funcs, nodes);
            continue;
          }
          case Global::Kind::EXTERN: {
            // TODO: follow externs.
            continue;
          }
          case Global::Kind::BLOCK: {
            continue;
          }
        }
        llvm_unreachable("invalid global kind");
      }
      for (auto *c : node.Called) {
        ctx_.MarkExecuted(*c);
      }
      ctx_.MarkExecuted(*f);
    } else {
      indirect = true;
    }
  }

  // If there are indirect calls, iterate until convergence.
  if (indirect) {
    std::set<Func *> visited;
    std::queue<Func *> qf;
    for (auto *f : funcs) {
      qf.push(f);
    }
    while (!qf.empty()) {
      auto *f = qf.front();
      qf.pop();
      if (!visited.insert(f).second) {
        continue;
      }

      LLVM_DEBUG(llvm::dbgs() << "Indirect call: " << f->getName() << "\n");
      ctx_.MarkExecuted(*f);

      auto &node = refs_[*f];
      raises = raises || node.HasRaise;
      for (auto *g : node.Referenced) {
        LLVM_DEBUG(llvm::dbgs() << "\t" << g->getName() << "\n");
        switch (g->GetKind()) {
          case Global::Kind::FUNC: {
            qf.push(static_cast<Func *>(g));
            continue;
          }
          case Global::Kind::ATOM: {
            auto *obj = static_cast<Atom *>(g)->getParent();
            std::set<Func *> fns;
            heap.Extract(obj, fns, nodes);
            for (auto *f : fns) {
              qf.push(f);
            }
            continue;
          }
          case Global::Kind::EXTERN: {
            // TODO: follow externs.
            continue;
          }
          case Global::Kind::BLOCK: {
            continue;
          }
        }
        llvm_unreachable("invalid global kind");
      }
      for (auto *c : node.Called) {
        ctx_.MarkExecuted(*c);
      }
    }
  }

  // Unify the pointer with all referenced items.
  refs.LUB(heap.Build(funcs, nodes));

  // Apply the effect of the transitive closure.
  bool changed = false;
  if (auto ptr = refs.AsPointer()) {
    changed = ctx_.Store(*ptr, refs, Type::I64);
  }
  return { changed, raises };
}

// -----------------------------------------------------------------------------
bool SymbolicApprox::Raise(const SymbolicValue &taint)
{
  // Taint all landing pads on the stack which can be reached from here.
  // Landing pads must be tainted with incoming values in case the
  // evaluation of an invoke instruction continues with the catch block.
  bool changed = false;
  if (auto ptr = taint.AsPointer()) {
    for (Block *blockPtr : ptr->blocks()) {
      for (auto &frame : ctx_.frames()) {
        // See whether the block is among the successors of the active node
        // in any of the frames on the stack, propagating to landing pads.
        auto *exec = frame.GetCurrentNode();
        if (!exec) {
          continue;
        }
        for (auto *succ : exec->Succs) {
          for (auto *block : succ->Blocks) {
            if (block != blockPtr) {
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
                changed = ctx_.Set(pad->GetSubValue(i), taint) || changed;
              }
            }
          }
        }
      }
    }
  }
  return changed;
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
      switch (::cast<Global>(arg)->GetKind()) {
        case Global::Kind::ATOM: {
          ctx_.Set(mov, SymbolicValue::Pointer(&*::cast<Atom>(arg), 0));
          return;
        }
        case Global::Kind::EXTERN: {
          ctx_.Set(mov, SymbolicValue::Pointer(&*::cast<Extern>(arg), 0));
          return;
        }
        case Global::Kind::FUNC: {
          ctx_.Set(mov, SymbolicValue::Pointer(&*::cast<Func>(arg)));
          return;
        }
        case Global::Kind::BLOCK: {
          ctx_.Set(mov, SymbolicValue::Pointer(&*::cast<Block>(arg)));
          return;
        }
      }
      llvm_unreachable("invalid global kind");
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
