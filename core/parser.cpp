// This file if part of the genm-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#include <cassert>
#include <optional>
#include <queue>
#include <sstream>
#include <stack>
#include <string_view>
#include <vector>

#include <llvm/ADT/SmallPtrSet.h>

#include "core/block.h"
#include "core/context.h"
#include "core/constant.h"
#include "core/data.h"
#include "core/dominator.h"
#include "core/func.h"
#include "core/insts.h"
#include "core/parser.h"
#include "core/prog.h"



// -----------------------------------------------------------------------------
class ParserError final : public std::exception {
public:
  /// Constructs a new error object.
  ParserError(unsigned row, unsigned col)
    : ParserError(row, col, "")
  {
  }

  /// Constructs a new error object.
  ParserError(unsigned row, unsigned col, const std::string_view &message)
  {
    std::ostringstream os;
    os << "[" << row << "," << col << "]: " << message;
    message_ = os.str();
  }

  /// Appends a string to the message.
  ParserError &operator << (const std::string_view &str)
  {
    message_ += str;
    return *this;
  }

  /// Appends a string to the message.
  ParserError &operator << (char chr)
  {
    std::ostringstream os;
    os << message_ << "\'" << chr << "\'";
    message_ = os.str();
    return *this;
  }

  /// Returns the error message.
  const char *what() const throw ()
  {
    return message_.c_str();
  }

private:
  /// Error message.
  std::string message_;
};



// -----------------------------------------------------------------------------
class ValidationError final : public std::exception {
public:
  /// Constructs a new error object.
  ValidationError(Func *func, const std::string_view &message)
  {
    std::ostringstream os;
    os << func->GetName() << ": " << message;
    message_ = os.str();
  }

  /// Constructs a new error object.
  ValidationError(Func *func, Block *block, const std::string_view &message)
  {
    std::ostringstream os;
    os << func->GetName() << "," << block->GetName() << ": " << message;
    message_ = os.str();
  }

  /// Returns the error message.
  const char *what() const throw ()
  {
    return message_.c_str();
  }

private:
  /// Error message.
  std::string message_;
};



// -----------------------------------------------------------------------------
static inline bool IsSpace(char chr)
{
  return chr == ' ' || chr == '\t' || chr == '\v';
}

// -----------------------------------------------------------------------------
static inline bool IsNewline(char chr)
{
  return chr == '\n';
}

// -----------------------------------------------------------------------------
static inline bool IsAlpha(char chr)
{
  return ('a' <= chr && chr <= 'z')
      || ('A' <= chr && chr <= 'Z')
      || chr == '_';
}

// -----------------------------------------------------------------------------
static inline bool IsDigit(char chr, unsigned base = 10)
{
  switch (base) {
    case 2: {
      return chr == '0' || chr == '1';
    }
    case 8: {
      return '0' <= chr && chr <= '7';
    }
    case 10: {
      return '0' <= chr && chr <= '9';
    }
    case 16: {
      return ('0' <= chr && chr <= '9')
          || ('a' <= chr && chr <= 'f')
          || ('A' <= chr && chr <= 'F');
    }
    default: {
      assert(!"invalid base");
    }
  }
}

// -----------------------------------------------------------------------------
static inline int ToInt(char chr)
{
  if ('0' <= chr && chr <= '9') {
    return chr - '0';
  }
  if ('a' <= chr && chr <= 'f') {
    return chr - 'a' + 10;
  }
  if ('A' <= chr && chr <= 'F') {
    return chr - 'A' + 10;
  }
  assert(!"invalid digit");
}

// -----------------------------------------------------------------------------
static inline bool IsAlphaNum(char chr)
{
  return IsAlpha(chr) || IsDigit(chr) || chr == '_';
}

// -----------------------------------------------------------------------------
static inline bool IsIdentStart(char chr)
{
  return IsAlpha(chr) || chr == '_' || chr == '.';
}

// -----------------------------------------------------------------------------
static inline bool IsIdentCont(char chr)
{
  return IsAlphaNum(chr) || chr == '$' || chr == '@';
}

// -----------------------------------------------------------------------------
Parser::Parser(Context &ctx, const std::string &path)
  : ctx_(ctx)
  , is_(path)
  , char_('\0')
  , tk_(Token::END)
  , row_(1)
  , col_(0)
  , prog_(new Prog)
  , data_(nullptr)
  , func_(nullptr)
  , block_(nullptr)
{
  if (!is_.good()) {
    throw ParserError(row_, col_, "Cannot open stream");
  }

  NextChar();
  NextToken();
}

// -----------------------------------------------------------------------------
Parser::~Parser()
{
}

// -----------------------------------------------------------------------------
Prog *Parser::Parse()
{
  while (tk_ != Token::END) {
    switch (tk_) {
      case Token::NEWLINE: {
        NextToken();
        continue;
      }
      case Token::LABEL: {
        if (data_ == nullptr) {
          if (!str_.empty() && str_[0] == '.') {
            // Start a new basic block.
            InFunc();
            auto it = blocks_.emplace(str_, nullptr);
            if (it.second) {
              // Block not declared yet - backward jump target.
              func_ = func_ ? func_ : prog_->AddFunc(*funcName_);
              block_ = new Block(func_, str_);
              it.first->second = block_;
            } else {
              // Block was created by a forward jump.
              block_ = it.first->second;
            }
            topo_.push_back(block_);
          } else {
            // Start a new function.
            if (func_) EndFunction();
            funcName_ = str_;
          }
        } else {
          // Pointer into the data segment.
          data_->CreateSymbol(str_);
        }
        Expect(Token::NEWLINE);
        continue;
      }
      case Token::IDENT: {
        if (!str_.empty() && str_[0] == '.') {
          ParseDirective();
        } else {
          ParseInstruction();
        }
        Check(Token::NEWLINE);
        continue;
      }
      default: {
        throw ParserError(row_, col_, "unexpected token, expected operation");
      }
    }
  }

  if (func_) EndFunction();
  return prog_;
}

// -----------------------------------------------------------------------------
Const *Parser::ParseValue()
{
  switch (tk_) {
    case Token::MINUS: {
      NextToken();
      Check(Token::NUMBER);
      NextToken();
      return nullptr;
    }
    case Token::NUMBER: {
      NextToken();
      return nullptr;
    }
    case Token::IDENT: {
      switch (NextToken()) {
        case Token::PLUS: {
          Expect(Token::NUMBER);
          NextToken();
          break;
        }
        case Token::MINUS: {
          Expect(Token::NUMBER);
          NextToken();
          break;
        }
        default: {
          break;
        }
      }
      return nullptr;
    }
    default: {
      throw ParserError(row_, col_, "unexpected token, expected value");
    }
  }
}

// -----------------------------------------------------------------------------
void Parser::ParseDirective()
{
  assert(str_.size() >= 2 && "empty directive");
  std::string op = str_;
  NextToken();

  switch (op[1]) {
    case 'a': {
      if (op == ".align") return ParseAlign();
      if (op == ".ascii") return ParseAscii();
      if (op == ".asciz") return ParseAsciz();
      if (op == ".args") return ParseArgs();
      break;
    }
    case 'b': {
      if (op == ".bss") return ParseBSS();
      if (op == ".byte") { InData(); return data_->AddInt8(ParseValue()); }
      break;
    }
    case 'c': {
      if (op == ".const") return ParseConst();
      if (op == ".call") return ParseCall();
      break;
    }
    case 'd': {
      if (op == ".data") return ParseData();
      if (op == ".double") { InData(); return data_->AddFloat64(ParseValue()); }
      break;
    }
    case 'e': {
      if (op == ".extern") return ParseExtern();
      break;
    }
    case 'l': {
      if (op == ".long") { InData(); return data_->AddInt32(ParseValue()); }
      break;
    }
    case 'q': {
      if (op == ".quad") { InData(); return data_->AddInt64(ParseValue()); }
      break;
    }
    case 's': {
      if (op == ".space") return ParseSpace();
      if (op == ".stack") return ParseStack();
      break;
    }
    case 't': {
      if (op == ".text") return ParseText();
      break;
    }
    case 'z': {
      if (op == ".zero") { InData(); return data_->AddZero(ParseValue()); }
      break;
    }
  }

  throw ParserError(row_, col_, "unknown directive: ") << op;
}

// -----------------------------------------------------------------------------
void Parser::ParseInstruction()
{
  // Make sure instruction is in text.
  InFunc();

  // Make sure we have a correct function.
  func_ = func_ ? func_ : prog_->AddFunc(*funcName_);

  // An instruction is composed of an opcode, followed by optional annotations.
  size_t dot = str_.find('.');
  std::string op = str_.substr(0, dot);

  std::optional<size_t> size;
  std::optional<Cond> cc;
  std::vector<Type> types;
  std::optional<CallingConv> conv;

  // Parse the tokens composing the opcode - size, condition code and types.
  while (dot != std::string::npos) {
    // Split the string at the next dot.
    size_t next = str_.find('.', dot + 1);
    size_t length = next == std::string::npos ? next : (next - dot - 1);
    std::string_view token = str_.substr(dot + 1, length);
    if (length == 0) {
      throw ParserError(row_, col_, "invalid opcode ") << str_;
    }
    dot = next;

    switch (token[0]) {
      case 'e': {
        if (token == "eq") { cc = Cond::EQ; continue; }
        break;
      }
      case 'l': {
        if (token == "lt") { cc = Cond::LT; continue; }
        if (token == "le") { cc = Cond::LE; continue; }
        break;
      }
      case 'g': {
        if (token == "gt") { cc = Cond::GT; continue; }
        if (token == "ge") { cc = Cond::GE; continue; }
        break;
      }
      case 'n': {
        if (token == "ne") { cc = Cond::NE; continue; }
        break;
      }
      case 'i': {
        if (token == "i8") { types.push_back(Type::I8); continue; }
        if (token == "i16") { types.push_back(Type::I16); continue; }
        if (token == "i32") { types.push_back(Type::I32); continue; }
        if (token == "i64") { types.push_back(Type::I64); continue; }
        break;
      }
      case 'f': {
        if (token == "f32") { types.push_back(Type::F32); continue; }
        if (token == "f64") { types.push_back(Type::F64); continue; }
        break;
      }
      case 'o': {
        if (token == "oeq") { cc = Cond::OEQ; continue; }
        if (token == "one") { cc = Cond::ONE; continue; }
        if (token == "olt") { cc = Cond::OLT; continue; }
        if (token == "ogt") { cc = Cond::OGT; continue; }
        if (token == "ole") { cc = Cond::OLE; continue; }
        if (token == "oge") { cc = Cond::OGE; continue; }
        break;
      }
      case 'u': {
        if (token == "u8") { types.push_back(Type::U8); continue; }
        if (token == "u16") { types.push_back(Type::U16); continue; }
        if (token == "u32") { types.push_back(Type::U32); continue; }
        if (token == "u64") { types.push_back(Type::U64); continue; }
        if (token == "ueq") { cc = Cond::UEQ; continue; }
        if (token == "une") { cc = Cond::UNE; continue; }
        if (token == "ult") { cc = Cond::ULT; continue; }
        if (token == "ugt") { cc = Cond::UGT; continue; }
        if (token == "ule") { cc = Cond::ULE; continue; }
        if (token == "uge") { cc = Cond::UGE; continue; }
        break;
      }
      default: {
        if (isdigit(token[0])) {
          // Parse integers, i.e. size operands.
          uint64_t sz = 0;
          for (size_t i = 0; i < token.size(); ++i) {
            if (!IsDigit(token[i])) {
              throw ParserError(row_, col_, "invalid opcode ") << str_;
            }
            sz = sz * 10 + ToInt(token[i]);
          }
          size = sz;
          continue;
        } else {
          break;
        }
      }
    }

    conv = ParseCallingConv(token);
  }

  // Parse all arguments.
  std::vector<Value *> ops;
  do {
    switch (NextToken()) {
      case Token::NEWLINE: {
        if (!ops.empty()) throw ParserError(row_, col_, "expected argument");
        break;
      }
      // $sp, $fp
      case Token::REG: {
        ops.emplace_back(ctx_.CreateReg(reg_));
        NextToken();
        break;
      }
      // $123
      case Token::VREG: {
        ops.emplace_back(reinterpret_cast<Inst *>((vreg_ << 1) | 1));
        NextToken();
        break;
      }
      // [$123] or [$sp]
      case Token::LBRACE: {
        switch (NextToken()) {
          case Token::REG: {
            ops.emplace_back(ctx_.CreateReg(reg_));
            break;
          }
          case Token::VREG: {
            ops.emplace_back(reinterpret_cast<Inst *>((vreg_ << 1) | 1));
            break;
          }
          default: {
            throw ParserError(row_, col_, "invalid indirection");
          }
        }
        Expect(Token::RBRACE);
        NextToken();
        break;
      }
      // -123
      case Token::MINUS: {
        Expect(Token::NUMBER);
        ops.emplace_back(ctx_.CreateInt(-int_));
        NextToken();
        break;
      }
      // 123
      case Token::NUMBER: {
        ops.emplace_back(ctx_.CreateInt(+int_));
        NextToken();
        break;
      }
      // _some_name + offset
      case Token::IDENT: {
        if (!str_.empty() && str_[0] == '.') {
          auto it = blocks_.emplace(str_, nullptr);
          if (it.second) {
            // Forward jump - create a placeholder block.
            it.first->second = new Block(func_, str_);
          }
          ops.emplace_back(it.first->second);
          NextToken();
        } else {
          Global *global = prog_->CreateSymbol(str_);
          switch (NextToken()) {
            case Token::PLUS: {
              Expect(Token::NUMBER);
              auto *sym = static_cast<Symbol *>(global);
              ops.emplace_back(ctx_.CreateSymbolOffset(sym, +int_));
              NextToken();
              break;
            }
            case Token::MINUS: {
              Expect(Token::NUMBER);
              auto *sym = static_cast<Symbol *>(global);
              ops.emplace_back(ctx_.CreateSymbolOffset(sym, -int_));
              NextToken();
              break;
            }
            default: {
              ops.emplace_back(global);
              break;
            }
          }
        }
        break;
      }
      default: {
        throw ParserError(row_, col_, "invalid argument");
      }
    }
  } while (tk_ == Token::COMMA);

  // Parse optional annotations.
  while (tk_ == Token::ANNOT) {
    NextToken();
  }

  // Done, must end with newline.
  Check(Token::NEWLINE);

  // Create a block for the instruction.
  if (block_ == nullptr) {
    // An empty start block, if not explicitly defined.
    block_ = new Block(func_, ".LBBentry");
    topo_.push_back(block_);
  } else if (!block_->IsEmpty()) {
    // If the previous instruction is a terminator, start a new block.
    Inst *l = &*block_->rbegin();
    if (l->IsTerminator()) {
      block_ = new Block(func_, ".LBBterm" + std::to_string(topo_.size()));
      topo_.push_back(block_);
    }
  }

  // Add the instruction to the block.
  Inst *i = CreateInst(op, ops, cc, size, types, conv);
  for (unsigned idx = 0, rets = i->GetNumRets(); idx < rets; ++idx) {
    const auto vreg = reinterpret_cast<uint64_t>(ops[idx]);
    vregs_[i] = vreg >> 1;
  }

  block_->AddInst(i);
}

// -----------------------------------------------------------------------------
void Parser::ParseBSS()
{
  if (func_) EndFunction();
  data_ = prog_->GetBSS();
}

// -----------------------------------------------------------------------------
void Parser::ParseData()
{
  if (func_) EndFunction();
  data_ = prog_->GetData();
}

// -----------------------------------------------------------------------------
void Parser::ParseConst()
{
  if (func_) EndFunction();
  data_ = prog_->GetConst();
}

// -----------------------------------------------------------------------------
void Parser::ParseText()
{
  if (func_) EndFunction();
  data_ = nullptr;
}

// -----------------------------------------------------------------------------
Inst *Parser::CreateInst(
    const std::string &opc,
    const std::vector<Value *> &ops,
    const std::optional<Cond> &ccs,
    const std::optional<size_t> &size,
    const std::vector<Type> &ts,
    const std::optional<CallingConv> &conv)
{
  auto val = [this, &ops](int idx) {
    if ((idx < 0 && -idx > ops.size()) || (idx >= 0 && idx >= ops.size())) {
      throw ParserError(row_, col_, "Missing operand");
    }
    return idx >= 0 ? ops[idx] : *(ops.end() + idx);
  };
  auto t = [this, &ts](int idx) {
    if ((idx < 0 && -idx > ts.size()) || (idx >= 0 && idx >= ts.size())) {
      throw ParserError(row_, col_, "Missing type");
    }
    return idx >= 0 ? ts[idx] : *(ts.end() + idx);
  };
  auto op = [this, &val](int idx) {
    Value *v = val(idx);
    if ((reinterpret_cast<uintptr_t>(v) & 1) == 0) {
      throw ParserError(row_, col_, "vreg expected");
    }
    return static_cast<Inst *>(v);
  };
  auto bb = [this, &val](int idx) {
    Value *v = val(idx);
    if ((reinterpret_cast<uintptr_t>(v) & 1) != 0 || !v->Is(Value::Kind::BLOCK)) {
      throw ParserError(row_, col_, "not a block");
    }
    return static_cast<Block *>(val(idx));
  };
  auto imm = [&val](int idx) {
    return static_cast<ConstantInt *>(val(idx));
  };
  auto reg = [&val](int idx) {
    return static_cast<ConstantReg *>(val(idx));
  };
  auto cc = [&ccs]() { return *ccs; };
  auto sz = [&size]() { return *size; };
  auto call = [this, &conv]() {
    if (!conv) {
      throw ParserError(row_, col_, "missing calling conv");
    }
    return *conv;
  };
  auto args = [this, &ops](int beg, int end) {
    std::vector<Inst *> args;
    for (auto it = ops.begin() + beg; it != ops.end() + end; ++it) {
      if ((reinterpret_cast<uintptr_t>(*it) & 1) == 0) {
        throw ParserError(row_, col_, "vreg expected");
      }
      args.push_back(static_cast<Inst *>(*it));
    }
    return args;
  };

  assert(opc.size() > 0 && "empty token");
  switch (opc[0]) {
    case 'a': {
      if (opc == "abs")  return new AbsInst(block_, t(0), op(1));
      if (opc == "add")  return new AddInst(block_, t(0), op(1), op(2));
      if (opc == "and")  return new AndInst(block_, t(0), op(1), op(2));
      if (opc == "arg")  return new ArgInst(block_, t(0), imm(1));
      break;
    }
    case 'c': {
      if (opc == "cmp")  return new CmpInst(block_, t(0), cc(), op(1), op(2));
      if (opc == "cos")  return new CosInst(block_, t(0), op(1));
      if (opc == "copysign") {
        return new CopySignInst(block_, t(0), op(1), op(2));
      }
      if (opc == "call") {
        if (ts.empty()) {
          return new CallInst(
              block_,
              op(0),
              args(1, 0),
              size ? *size : ops.size() - 1,
              call()
          );
        } else {
          return new CallInst(
              block_,
              t(0),
              op(1),
              args(2, 0),
              size ? *size : ops.size() - 2,
              call()
          );
        }
      }
      break;
    }
    case 'd': {
      if (opc == "div") return new DivInst(block_, t(0), op(1), op(2));
      break;
    }
    case 'i': {
      if (opc == "invoke") {
        if (ts.empty()) {
          return new InvokeInst(
              block_,
              t(0),
              op(0),
              args(1, -1),
              nullptr,
              bb(-1),
              ops.size() - 2,
              call()
          );
        } else {
          return new InvokeInst(
              block_,
              t(0),
              op(1),
              args(2, -1),
              nullptr,
              bb(-1),
              ops.size() - 3,
              call()
          );
        }
      }
      break;
    }
    case 'f': {
      if (opc == "fext")   return new FExtInst(block_, t(0), op(1));
      if (opc == "frame")  return new FrameInst(block_, t(0), imm(1));
      break;
    }
    case 'j': {
      if (opc == "jf")  return new JumpCondInst(block_, op(0), bb(1), nullptr);
      if (opc == "jt")  return new JumpCondInst(block_, op(0), nullptr, bb(1));
      if (opc == "ji")  return new JumpIndirectInst(block_, op(0));
      if (opc == "jmp") return new JumpInst(block_, bb(0));
      break;
    }
    case 'l': {
      if (opc == "ld") return new LoadInst(block_, sz(), t(0), op(1));
      break;
    }
    case 'm': {
      if (opc == "mov") return new MovInst(block_, t(0), val(1));
      if (opc == "mul") return new MulInst(block_, t(0), op(1), op(2));
      break;
    }
    case 'n': {
      if (opc == "neg") return new NegInst(block_, t(0), op(1));
      break;
    }
    case 'o': {
      if (opc == "or") return new OrInst(block_, t(0), op(1), op(2));
      break;
    }
    case 'p': {
      if (opc == "pow")  return new PowInst(block_, t(0), op(1), op(2));
      if (opc == "phi") {
        if ((ops.size() & 1) == 0) {
          throw ParserError(row_, col_, "Invalid PHI instruction");
        }
        PhiInst *phi = new PhiInst(block_, t(0));
        for (unsigned i = 1; i < ops.size(); i += 2) {
          phi->Add(bb(i), ops[i + 1]);
        }
        return phi;
      }
      break;
    }
    case 'r': {
      if (opc == "rem")  return new RemInst(block_, t(0), op(1), op(2));
      if (opc == "rotl") return new RotlInst(block_, t(0), op(1), op(2));
      if (opc == "ret") {
        if (ts.empty()) {
          return new ReturnInst(block_);
        } else {
          return new ReturnInst(block_, t(0), op(0));
        }
      }
      break;
    }
    case 's': {
      if (opc == "set")    return new SetInst(block_, reg(0), op(1));
      if (opc == "sext")   return new SExtInst(block_, t(0), op(1));
      if (opc == "sll")    return new SllInst(block_, t(0), op(1), op(2));
      if (opc == "sra")    return new SraInst(block_, t(0), op(1), op(2));
      if (opc == "srl")    return new SrlInst(block_, t(0), op(1), op(2));
      if (opc == "st")     return new StoreInst(block_, sz(), op(0), op(1));
      if (opc == "sub")    return new SubInst(block_, t(0), op(1), op(2));
      if (opc == "sqrt")   return new SqrtInst(block_, t(0), op(1));
      if (opc == "sin")    return new SinInst(block_, t(0), op(1));
      if (opc == "select") {
        return new SelectInst(block_, t(0), op(1), op(2), op(3));
      }
      if (opc == "switch") {
        return new SwitchInst(block_, op(0), { ops.begin() + 1, ops.end() });
      }
      break;
    }
    case 't': {
      if (opc == "trunc") return new TruncInst(block_, t(0), op(1));
      if (opc == "trap")  return new TrapInst(block_);
      if (opc == "tcall") {
        if (ts.empty()) {
          return new TailCallInst(
              block_,
              op(0),
              args(1, 0),
              size.value_or(ops.size() - 2),
              call()
          );
        } else {
          return new TailCallInst(
              block_,
              t(0),
              op(0),
              args(1, 0),
              size.value_or(ops.size() - 2),
              call()
          );
        }
      }
      break;
    }
    case 'u': {
      if (opc == "undef") return new UndefInst(block_, t(0));
      break;
    }
    case 'v': {
      if (opc == "vastart") return new VAStartInst(block_, op(0));
      break;
    }
    case 'x': {
      if (opc == "xchg") return new ExchangeInst(block_, t(0), op(1), op(2));
      if (opc == "xor")  return new XorInst(block_, t(0), op(1), op(2));
      break;
    }
    case 'z': {
      if (opc == "zext") return new ZExtInst(block_, t(0), op(1));
      break;
    }
  }

  throw ParserError(row_, col_, "unknown opcode: ") << opc;
}

// -----------------------------------------------------------------------------
Func *Parser::GetFunction()
{
  func_ = func_ ? func_ : prog_->AddFunc(*funcName_);
  if (align_) {
    func_->SetAlignment(*align_);
    align_ = std::nullopt;
  }
  return func_;
}

// -----------------------------------------------------------------------------
void Parser::EndFunction()
{
  // Add the blocks to the function, in order. Add jumps to blocks which
  // fall through and fix the fall-through branches of conditionals.
  for (auto it = topo_.begin(); it != topo_.end(); ++it) {
    Block *block = *it;
    if (auto term = block->GetTerminator()) {
      for (Use &use : term->operands()) {
        if (use == nullptr) {
          if (it + 1 == topo_.end()) {
            throw ValidationError(func_, "Jump falls through");
          } else {
            use = *(it + 1);
          }
        }
      }
    } else if (it + 1 != topo_.end()) {
      block->AddInst(new JumpInst(block, *(it + 1)));
    } else {
      throw ValidationError(func_, "Unterminated function");
    }
    func_->AddBlock(block);
  }

  // Construct the dominator tree & find dominance frontiers.
  DominatorTree DT(*func_);
  DominanceFrontier DF;
  DF.analyze(DT);

  // Placement of PHI nodes.
  {
    // Find all definitions of all variables.
    llvm::DenseMap<unsigned, std::queue<Inst *>> sites;
    for (Block &block : *func_) {
      for (Inst &inst : block) {
        if (inst.GetNumRets() > 0) {
          sites[vregs_[&inst]].push(&inst);
        }
      }
    }

    // Find the dominance frontier of the blocks where variables are defined.
    // Place PHI nodes at the start of those blocks, continuing with the
    // dominance frontier of those nodes iteratively.
    for (auto &var : sites) {
      auto &q = var.second;
      while (!q.empty()) {
        auto *inst = q.front();
        q.pop();
        auto *block = inst->getParent();
        if (auto *node = DT.getNode(block)) {
          for (auto &front : DF.calculate(DT, node)) {
            bool found = false;
            for (PhiInst &phi : front->phis()) {
              if (vregs_[&phi] == var.first) {
                found = true;
                break;
              }
            }

            // If the PHI node was not added already, add it.
            if (!found) {
              auto *phi = new PhiInst(front, inst->GetType(0));
              front->AddPhi(phi);
              vregs_[phi] = var.first;
              q.push(phi);
            }
          }
        }
      }
    }

    // Renaming variables to point to definitions or PHI nodes.
    llvm::DenseMap<unsigned, std::stack<Inst *>> vars;
    std::function<void(Block *block)> rename = [&](Block *block) {
      // Rename variables in this block, capturing definitions on the stack.
      for (Inst &inst : *block) {
        for (Use &use : inst.operands()) {
          const auto vreg = reinterpret_cast<uint64_t>(use.get());
          if (vreg & 1) {
            auto &stk = vars[vreg >> 1];
            if (stk.empty()) {
              throw ValidationError(
                  func_,
                  block,
                  "undefined vreg: " + std::to_string(vreg >> 1)
              );
            }
            use = stk.top();
          }
        }

        if (inst.GetNumRets() > 0) {
          auto it = vregs_.find(&inst);
          if (it != vregs_.end()) {
            vars[it->second].push(&inst);
          }
        }
      }

      // Handle PHI nodes in successors.
      for (Block *succ : block->successors()) {
        for (PhiInst &phi : succ->phis()) {
          auto &stk = vars[vregs_[&phi]];
          if (!stk.empty()) {
            phi.Add(block, stk.top());
          } else if (!phi.HasValue(block)) {
            auto *undef = new UndefInst(block, phi.GetType());
            block->AddInst(undef, block->GetTerminator());
            phi.Add(block, undef);
          }
        }
      }

      // Recursively rename child nodes.
      for (const auto *child : *DT[block]) {
        rename(child->getBlock());
      }

      // Pop definitions of this block from the stack.
      for (Inst &inst : *block) {
        if (inst.GetNumRets() > 0) {
          auto it = vregs_.find(&inst);
          if (it != vregs_.end()) {
            vars[it->second].pop();
          }
        }
      }
    };
    rename(DT.getRoot());
  }

  func_ = nullptr;
  block_ = nullptr;

  vregs_.clear();
  blocks_.clear();
  topo_.clear();
}

// -----------------------------------------------------------------------------
void Parser::ParseAlign()
{
  Check(Token::NUMBER);
  if (data_) {
    data_->Align(int_);
  } else {
    if (func_) {
      throw ParserError(row_, col_, "Cannot align instructions in functions");
    } else {
      align_ = int_;
    }
  }
  Expect(Token::NEWLINE);
}

// -----------------------------------------------------------------------------
void Parser::ParseExtern()
{
  Check(Token::IDENT);
  prog_->AddExternal(str_);
  Expect(Token::NEWLINE);
}

// -----------------------------------------------------------------------------
void Parser::ParseSpace()
{
  Check(Token::NUMBER);
  Expect(Token::NEWLINE);
}

// -----------------------------------------------------------------------------
void Parser::ParseStack()
{
  Check(Token::NUMBER);
  if (!funcName_) {
    throw ParserError(row_, col_, "stack directive not in function");
  }
  GetFunction()->SetStackSize(int_);
  Expect(Token::NEWLINE);
}

// -----------------------------------------------------------------------------
void Parser::ParseCall()
{
  Check(Token::IDENT);
  if (!funcName_) {
    throw ParserError(row_, col_, "stack directive not in function");
  }
  GetFunction()->SetCallingConv(ParseCallingConv(str_));
  Expect(Token::NEWLINE);
}

// -----------------------------------------------------------------------------
void Parser::ParseArgs()
{
  Check(Token::NUMBER);
  if (!funcName_) {
    throw ParserError(row_, col_, "stack directive not in function");
  }

  auto *func = GetFunction();
  func->SetVarArg(int_ != 0);
  std::vector<Type> types;
  while (NextToken() == Token::COMMA) {
    Expect(Token::IDENT);
    switch (str_[0]) {
      case 'i': {
        if (str_ == "i8") { types.push_back(Type::I8); continue; }
        if (str_ == "i16") { types.push_back(Type::I16); continue; }
        if (str_ == "i32") { types.push_back(Type::I32); continue; }
        if (str_ == "i64") { types.push_back(Type::I64); continue; }
        break;
      }
      case 'f': {
        if (str_ == "f32") { types.push_back(Type::F32); continue; }
        if (str_ == "f64") { types.push_back(Type::F64); continue; }
        break;
      }
      default: {
        throw ParserError(row_, col_, "invalid type");
      }
    }
  }
  Check(Token::NEWLINE);
  func_->SetParameters(types);
}

// -----------------------------------------------------------------------------
void Parser::ParseAscii()
{
  Check(Token::STRING);
  Expect(Token::NEWLINE);
}

// -----------------------------------------------------------------------------
void Parser::ParseAsciz()
{
  Check(Token::STRING);
  Expect(Token::NEWLINE);
}

// -----------------------------------------------------------------------------
void Parser::InData()
{
  if (data_ == nullptr || func_ != nullptr) {
    throw ParserError(row_, col_, "not in a data segment");
  }
}

// -----------------------------------------------------------------------------
void Parser::InFunc()
{
  if (data_ != nullptr || !funcName_) {
    throw ParserError(row_, col_, "not in a text segment");
  }
}

// -----------------------------------------------------------------------------
CallingConv Parser::ParseCallingConv(const std::string_view str)
{
  if (str == "c") {
    return CallingConv::C;
  } else if (str == "fast") {
    return CallingConv::FAST;
  } else if (str == "ocaml") {
    return CallingConv::OCAML;
  } else {
    throw ParserError(
        row_,
        col_,
        "unknown calling convention " + std::string(str)
    );
  }
}

// -----------------------------------------------------------------------------
Parser::Token Parser::NextToken()
{
  // Clear the value buffer.
  str_.clear();
  int_ = 0;

  // Skip whitespaces and newlines, coalesce multiple newlines into one.
  bool isNewline = false;
  while (IsSpace(char_) || IsNewline(char_) || char_ == '#') {
    while (IsSpace(char_)) NextChar();
    if (char_ == '#') {
      while (NextChar() != '\n');
    }
    if (char_ == '\n') {
      isNewline = true;
      NextChar();
      continue;
    }
  }
  if (isNewline) {
    return tk_ = Token::NEWLINE;
  }

  // Anything but newline.
  switch (char_) {
    case '\0': return tk_ = Token::END;
    case '[': NextChar(); return tk_ = Token::LBRACE;
    case ']': NextChar(); return tk_ = Token::RBRACE;
    case ',': NextChar(); return tk_ = Token::COMMA;
    case '+': NextChar(); return tk_ = Token::PLUS;
    case '-': NextChar(); return tk_ = Token::MINUS;
    case '$': {
      NextChar();
      if (IsDigit(char_)) {
        vreg_ = 0ull;
        do {
          vreg_ = vreg_ * 10 + ToInt(char_);
        } while (IsDigit(NextChar()));
        return tk_ = Token::VREG;
      } else if (IsAlpha(char_)) {
        do {
          str_.push_back(char_);
        } while (IsAlphaNum(NextChar()));
        if (str_ == "sp") { reg_ = ConstantReg::Kind::SP; return tk_ = Token::REG; };
        throw ParserError(row_, col_, "unknown register");
      } else {
        throw ParserError(row_, col_, "invalid register name");
      }
      return tk_ = Token::REG;
    }
    case '@': {
      NextChar();
      if (!IsAlphaNum(NextChar())) {
        throw ParserError(row_, col_, "empty annotation");
      }
      do {
        str_.push_back(char_);
      } while (IsAlphaNum(NextChar()) || char_ == '.');
      return tk_ = Token::ANNOT;
    }
    case '\"': {
      NextChar();
      while (char_ != '\"') {
        if (char_ == '\\') {
          str_.push_back(char_);
          str_.push_back(NextChar());
        } else {
          str_.push_back(char_);
        }
        NextChar();
      }
      NextChar();
      return tk_ = Token::STRING;
    }
    default: {
      if (IsIdentStart(char_)) {
        do {
          str_.push_back(char_);
        } while (IsIdentCont(NextChar()) || char_ == '.');

        if (char_ == ':') {
          NextChar();
          return tk_ = Token::LABEL;
        } else {
          return tk_ = Token::IDENT;
        }
      } else if (IsDigit(char_)) {
        unsigned base = 10;
        if (char_ == '0') {
          switch (NextChar()) {
            case 'x': base = 16; NextChar(); break;
            case 'b': base =  2; NextChar(); break;
            case 'o': base =  8; NextChar(); break;
            default: {
              if (IsDigit(char_)) {
                throw ParserError(row_, col_, "invalid numeric constant");
              }
              return tk_ = Token::NUMBER;
            }
          }
        }
        do {
          int_ = int_ * base + ToInt(char_);
        } while (IsDigit(NextChar(), base));
        if (IsAlphaNum(char_)) {
          throw ParserError(row_, col_, "invalid numeric constant");
        }
        return tk_ = Token::NUMBER;
      } else {
        throw ParserError(row_, col_, "unexpected character: ") << char_;
      }
    }
  }

  return Token::END;
}

// -----------------------------------------------------------------------------
char Parser::NextChar()
{
  char ch = is_.get();
  if (IsNewline(ch)) {
    row_ += 1;
    col_ = 1;
  } else {
    col_ += 1;
  }
  char_ = ch == EOF ? '\0' : ch;
  return char_;
}

// -----------------------------------------------------------------------------
void Parser::Expect(Token type)
{
  NextToken();
  Check(type);
}

// -----------------------------------------------------------------------------
void Parser::Check(Token type)
{
  if (tk_ != type) {
    auto ToString = [](Token tk) {
      switch (tk) {
        case Token::NEWLINE:  return "newline";
        case Token::END:      return "eof";
        case Token::LBRACE:   return "'['";
        case Token::RBRACE:   return "']'";
        case Token::COMMA:    return "','";
        case Token::REG:      return "reg";
        case Token::VREG:     return "vreg";
        case Token::IDENT:    return "identifier";
        case Token::LABEL:    return "label";
        case Token::NUMBER:   return "number";
        case Token::ANNOT:    return "annot";
        case Token::STRING:   return "string";
        case Token::PLUS:     return "'+'";
        case Token::MINUS:    return "'-'";
      }
    };
    throw ParserError(row_, col_)
        << ToString(type) << " expected, got " << ToString(tk_);
  }
}
;
