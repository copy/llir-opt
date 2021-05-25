// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#pragma once

#include <queue>
#include <unordered_set>

#include "core/inst_visitor.h"
#include "core/target.h"
#include "core/analysis/dominator.h"
#include "passes/tags/tagged_type.h"



namespace tags {

class TypeAnalysis;

/**
 * Helper to produce the initial types for known values.
 */
class Refinement : public InstVisitor<void> {
public:
  Refinement(TypeAnalysis &analysis, const Target *target, Func &func);

  void Run();

private:
  /// Refine a type to a more precise one.
  void Refine(Inst &inst, Block *parent, Ref<Inst> ref, const TaggedType &type);
  /// Refine a reference to an address.
  void RefineAddr(Inst &inst, Ref<Inst> addr);

private:
  void VisitMemoryLoadInst(MemoryLoadInst &i) override;
  void VisitMemoryStoreInst(MemoryStoreInst &i) override;
  void VisitSubInst(SubInst &i) override;
  void VisitAddInst(AddInst &i) override;
  void VisitAndInst(AndInst &i) override;
  void VisitOrInst(OrInst &i) override;
  void VisitXorInst(XorInst &i) override;
  void VisitCmpInst(CmpInst &i) override;
  void VisitMovInst(MovInst &i) override;
  void VisitPhiInst(PhiInst &phi) override;
  void VisitCallSite(CallSite &site) override;
  void VisitInst(Inst &i) override {}

private:
  /// Reference to the analysis.
  TypeAnalysis &analysis_;
  /// Reference to target info.
  const Target *target_;
  /// Function to optimise.
  Func &func_;
  /// Dominator tree.
  DominatorTree dt_;
  /// Dominance frontier.
  DominanceFrontier df_;
  /// Post-Dominator Tree.
  PostDominatorTree pdt_;
  /// Post-Dominance Frontier.
  PostDominanceFrontier pdf_;
  /// Queue of instructions to simplify.
  std::queue<Inst *> queue_;
  /// Set of items in the queue.
  std::unordered_set<Inst *> inQueue_;
};

} // end namespace
