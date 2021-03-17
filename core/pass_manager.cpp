// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include <chrono>
#include <iostream>

#include <llvm/Support/Format.h>

#include "core/pass.h"
#include "core/pass_manager.h"
#include "core/printer.h"



// -----------------------------------------------------------------------------
void PassManager::Run(Prog &prog)
{
  for (auto &group : groups_) {
    bool changed;
    do {
      changed = false;
      if (group.Passes.size() > 1 && time_ && verbose_) {
        llvm::outs() << "-----------\n";
      }
      for (auto &pass : group.Passes) {
        if (Run(pass, prog)) {
          changed = true;
          analyses_.clear();
        }
      }
    } while (group.Repeat && changed);
  }

  if (time_) {
    size_t length = 0;
    for (auto &[key, times] : times_) {
      length = std::max(length, strlen(key));
    }
    llvm::outs() << "\n";
    llvm::outs() << "===" << std::string(73, '-') << "===";
    llvm::outs() << "\n";
    for (auto &[key, times] : times_) {
      llvm::outs() << key << ": ";
      for (size_t i = 0; i < length - strlen(key); ++i) {
        llvm::outs() << ' ';
      }

      double sum = 0.0f, sqsum = 0.0f;
      for (auto time :  times) {
        sum += time;
        sqsum += time * time;
      }
      const size_t n = times.size();
      const double mean = sum / n;
      const double stddev = std::sqrt(sqsum / n - mean * mean);
      llvm::outs() << llvm::format("%10.2f ± %4.2f", mean, stddev) << "\n";
    }
    llvm::outs() << "===" << std::string(73, '-') << "===";
    llvm::outs() << "\n\n";
  }
}

// -----------------------------------------------------------------------------
bool PassManager::Run(PassInfo &pass, Prog &prog)
{
  const auto &name = pass.P->GetPassName();

  // Print information.
  if (time_ && verbose_) {
    llvm::outs() << name << ": ";
  }

  // Run the pass, measuring elapsed time.
  double elapsed;
  bool changed;
  {
    const auto start = std::chrono::high_resolution_clock::now();
    changed = pass.P->Run(prog);
    const auto end = std::chrono::high_resolution_clock::now();

    elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        end - start
    ).count() / 1e6;
  }

  // If timed, print duration.
  if (time_ && verbose_) {
    llvm::outs() << llvm::format("%.5f", elapsed) << "s";
    if (changed) {
      llvm::outs() << ", changed";
    }
    llvm::outs() << "\n";
  }

  // Record the analysis results.
  if (pass.ID) {
    analyses_.emplace(pass.ID, pass.P.get());
  }

  // Record running time.
  times_[pass.P->GetPassName()].push_back(elapsed);

  return changed;
}
