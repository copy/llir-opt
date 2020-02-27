// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#pragma once

#include <cstdint>

#include "core/value.h"
#include "core/symbol.h"

class Context;
class Symbol;



/**
 * Expression operand.
 */
class Expr : public User {
public:
  /// Enumeration of expression kinds.
  enum Kind {
    /// Fixed offset starting at a symbol.
    SYMBOL_OFFSET,
  };

  /// Destroys the expression.
  ~Expr();

  /// Returns the expression kind.
  Kind GetKind() const { return kind_; }

protected:
  /// Constructs a new expression.
  Expr(Kind kind, unsigned numOps)
    : User(Value::Kind::EXPR, numOps)
    , kind_(kind)
  {
  }

private:
  /// Expression kind.
  const Kind kind_;
};


/**
 * Symbol offset expression.
 */
class SymbolOffsetExpr final : public Expr {
public:
  /// Creates a new symbol offset expression.
  SymbolOffsetExpr(Global *sym, int64_t offset)
    : Expr(Kind::SYMBOL_OFFSET, 1)
    , offset_(offset)
  {
    Op<0>() = sym;
  }

  /// Returns the symbol.
  Global *GetSymbol() const { return static_cast<Global *>(Op<0>().get()); }
  /// Returns the offset.
  int64_t GetOffset() const { return offset_; }

private:
  /// Offset into the symbol.
  int64_t offset_;
};
