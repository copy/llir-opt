// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#pragma once

#include "core/pass.h"

class Func;
class CallInst;



/**
 * Function inliner pass.
 */
class InlinerPass final : public Pass {
public:
  /// Pass identifier.
  static const char *kPassID;

  /// Initialises the pass.
  InlinerPass(PassManager *passManager) : Pass(passManager) {}

  /// Runs the pass.
  void Run(Prog *prog) override;

  /// Returns the name of the pass.
  const char *GetPassName() const override;
};
