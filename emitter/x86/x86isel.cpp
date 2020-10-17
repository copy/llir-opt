// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.


#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Mangler.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/CodeGen/MachineInstrBuilder.h>
#include <llvm/CodeGen/MachineJumpTableInfo.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/SelectionDAGISel.h>
#include <llvm/Target/X86/X86ISelLowering.h>

#include "core/block.h"
#include "core/cast.h"
#include "core/cfg.h"
#include "core/data.h"
#include "core/extern.h"
#include "core/func.h"
#include "core/inst.h"
#include "core/insts.h"
#include "core/prog.h"
#include "core/analysis/dominator.h"
#include "emitter/x86/x86call.h"
#include "emitter/x86/x86isel.h"

namespace ISD = llvm::ISD;
namespace X86 = llvm::X86;
namespace X86ISD = llvm::X86ISD;
using MVT = llvm::MVT;
using EVT = llvm::EVT;
using GlobalValue = llvm::GlobalValue;
using SDNodeFlags = llvm::SDNodeFlags;
using SDNode = llvm::SDNode;
using SDValue = llvm::SDValue;
using SDVTList = llvm::SDVTList;
using SelectionDAG = llvm::SelectionDAG;
using X86RegisterInfo = llvm::X86RegisterInfo;
using GlobalAddressSDNode = llvm::GlobalAddressSDNode;
using ConstantSDNode = llvm::ConstantSDNode;


// -----------------------------------------------------------------------------
char X86ISel::ID;

// -----------------------------------------------------------------------------
X86ISel::X86ISel(
    llvm::X86TargetMachine *TM,
    llvm::X86Subtarget *STI,
    const llvm::X86InstrInfo *TII,
    const llvm::X86RegisterInfo *TRI,
    const llvm::TargetLowering *TLI,
    llvm::TargetLibraryInfo *LibInfo,
    const Prog *prog,
    llvm::CodeGenOpt::Level OL,
    bool shared)
  : DAGMatcher(*TM, new llvm::SelectionDAG(*TM, OL), OL, TLI, TII)
  , X86DAGMatcher(*TM, OL, STI)
  , ISel(ID, prog, LibInfo)
  , TM_(TM)
  , TRI_(TRI)
  , trampoline_(nullptr)
  , shared_(shared)
{
}

// -----------------------------------------------------------------------------
void X86ISel::LowerArch(const Inst *i)
{
  switch (i->GetKind()) {
    default: {
      llvm_unreachable("invalid architecture-specific instruction");
      return;
    }
    case Inst::Kind::X86_XCHG:    return LowerXchg(static_cast<const X86_XchgInst *>(i));
    case Inst::Kind::X86_CMPXCHG: return LowerCmpXchg(static_cast<const X86_CmpXchgInst *>(i));
    case Inst::Kind::X86_RDTSC:   return LowerRDTSC(static_cast<const X86_RdtscInst *>(i));
    case Inst::Kind::X86_FNCLEX:  return LowerFnClEx(static_cast<const X86_FnClExInst *>(i));
    case Inst::Kind::X86_FNSTCW:  return LowerFPUControl(X86ISD::FNSTCW16m, 2, true, i);
    case Inst::Kind::X86_FNSTSW:  return LowerFPUControl(X86ISD::FNSTSW16m, 2, true, i);
    case Inst::Kind::X86_FNSTENV: return LowerFPUControl(X86ISD::FNSTENVm, 28, true, i);
    case Inst::Kind::X86_FLDCW:   return LowerFPUControl(X86ISD::FLDCW16m, 2, false, i);
    case Inst::Kind::X86_FLDENV:  return LowerFPUControl(X86ISD::FLDENVm, 28, false, i);
    case Inst::Kind::X86_LDMXCSR: return LowerFPUControl(X86ISD::LDMXCSR32m, 4, false, i);
    case Inst::Kind::X86_STMXCSR: return LowerFPUControl(X86ISD::STMXCSR32m, 4, true, i);
  }
}

// -----------------------------------------------------------------------------
void X86ISel::LowerFnClEx(const X86_FnClExInst *)
{
  SDValue Ops[] = { CurDAG->getRoot() };
  CurDAG->setRoot(
      CurDAG->getNode(
          X86ISD::FNCLEX,
          SDL_,
          CurDAG->getVTList(MVT::Other),
          Ops
      )
  );
}

// -----------------------------------------------------------------------------
void X86ISel::LowerReturn(const ReturnInst *retInst)
{
  llvm::SmallVector<SDValue, 6> returns;
  returns.push_back(SDValue());
  returns.push_back(CurDAG->getTargetConstant(0, SDL_, MVT::i32));

  SDValue flag;
  SDValue chain = GetExportRoot();
  if (auto *retVal = retInst->GetValue()) {
    Type retType = retVal->GetType(0);
    unsigned retReg;
    switch (retType) {
      case Type::I8:  retReg = X86::AL;   break;
      case Type::I16: retReg = X86::AX;   break;
      case Type::I64: retReg = X86::RAX;  break;
      case Type::I32: retReg = X86::EAX;  break;
      case Type::F32: retReg = X86::XMM0; break;
      case Type::F64: retReg = X86::XMM0; break;
      case Type::F80: retReg = X86::FP0;  break;
      default: Error(retInst, "Invalid return type");
    }

    SDValue arg = GetValue(retVal);
    chain = CurDAG->getCopyToReg(chain, SDL_, retReg, arg, flag);
    returns.push_back(CurDAG->getRegister(retReg, GetType(retType)));
    flag = chain.getValue(1);
  }

  returns[0] = chain;
  if (flag.getNode()) {
    returns.push_back(flag);
  }

  CurDAG->setRoot(CurDAG->getNode(X86ISD::RET_FLAG, SDL_, MVT::Other, returns));
}

// -----------------------------------------------------------------------------
void X86ISel::LowerCmpXchg(const X86_CmpXchgInst *inst)
{
  unsigned reg;
  unsigned size;
  MVT type;
  switch (inst->GetType()) {
    case Type::I8:  reg = X86::AL;  size = 1; type = MVT::i8;  break;
    case Type::I16: reg = X86::AX;  size = 2; type = MVT::i16; break;
    case Type::I32: reg = X86::EAX; size = 4; type = MVT::i32; break;
    case Type::I64: reg = X86::RAX; size = 8; type = MVT::i64; break;
    case Type::I128: {
      Error(inst, "invalid type for atomic");
    }
    case Type::F32: case Type::F64: case Type::F80: {
      Error(inst, "invalid type for atomic");
    }
  }

  auto *mmo = MF->getMachineMemOperand(
      llvm::MachinePointerInfo(static_cast<llvm::Value *>(nullptr)),
      llvm::MachineMemOperand::MOVolatile |
      llvm::MachineMemOperand::MOLoad |
      llvm::MachineMemOperand::MOStore,
      GetSize(inst->GetType()),
      llvm::Align(size),
      llvm::AAMDNodes(),
      nullptr,
      llvm::SyncScope::System,
      llvm::AtomicOrdering::SequentiallyConsistent,
      llvm::AtomicOrdering::SequentiallyConsistent
  );

  SDValue writeReg = CurDAG->getCopyToReg(
      CurDAG->getRoot(),
      SDL_,
      reg,
      GetValue(inst->GetRef()),
      SDValue()
  );
  SDValue ops[] = {
      writeReg.getValue(0),
      GetValue(inst->GetAddr()),
      GetValue(inst->GetVal()),
      CurDAG->getTargetConstant(size, SDL_, MVT::i8),
      writeReg.getValue(1)
   };
  SDValue cmpXchg = CurDAG->getMemIntrinsicNode(
      X86ISD::LCMPXCHG_DAG,
      SDL_,
      CurDAG->getVTList(MVT::Other, MVT::Glue),
      ops,
      type,
      mmo
  );
  SDValue readReg = CurDAG->getCopyFromReg(
      cmpXchg.getValue(0),
      SDL_,
      reg,
      type,
      cmpXchg.getValue(1)
  );
  CurDAG->setRoot(readReg.getValue(1));
  Export(inst, readReg.getValue(0));
}

// -----------------------------------------------------------------------------
void X86ISel::LowerSet(const SetInst *inst)
{
  auto value = GetValue(inst->GetValue());
  switch (inst->GetReg()->GetValue()) {
    // Stack pointer.
    case ConstantReg::Kind::SP: {
      return LowerSetSP(value);
    }
    // TLS base.
    case ConstantReg::Kind::FS: {
      Error(inst, "Cannot rewrite tls base");
    }
    // Program counter.
    case ConstantReg::Kind::PC: {
      Error(inst, "Cannot rewrite program counter");
    }
    // Frame address.
    case ConstantReg::Kind::FRAME_ADDR: {
      Error(inst, "Cannot rewrite frame address");
    }
    // Return address.
    case ConstantReg::Kind::RET_ADDR: {
      Error(inst, "Cannot rewrite return address");
    }
  }
}

// -----------------------------------------------------------------------------
void X86ISel::LowerVAStart(const VAStartInst *inst)
{
  if (!inst->getParent()->getParent()->IsVarArg()) {
    Error(inst, "vastart in a non-vararg function");
  }

  CurDAG->setRoot(CurDAG->getNode(
      ISD::VASTART,
      SDL_,
      MVT::Other,
      CurDAG->getRoot(),
      GetValue(inst->GetVAList()),
      CurDAG->getSrcValue(nullptr)
  ));
}

// -----------------------------------------------------------------------------
void X86ISel::LowerRDTSC(const X86_RdtscInst *inst)
{
  switch (inst->GetType()) {
    case Type::I8:
    case Type::I16:
    case Type::I32: {
      llvm_unreachable("not implemented");
    }
    case Type::I64: {
      SDVTList Tys = CurDAG->getVTList(MVT::Other, MVT::Glue);
      SDValue Read = SDValue(CurDAG->getMachineNode(
          X86::RDTSC,
          SDL_,
          Tys,
          CurDAG->getRoot()
      ), 0);

      SDValue LO = CurDAG->getCopyFromReg(
          Read,
          SDL_,
          X86::RAX,
          MVT::i64,
          Read.getValue(1)
      );
      SDValue HI = CurDAG->getCopyFromReg(
          LO.getValue(1),
          SDL_,
          X86::RDX,
          MVT::i64,
          LO.getValue(2)
      );

      SDValue TSC = CurDAG->getNode(
          ISD::OR,
          SDL_,
          MVT::i64,
          LO,
          CurDAG->getNode(
              ISD::SHL,
              SDL_,
              MVT::i64, HI,
              CurDAG->getConstant(32, SDL_, MVT::i8)
          )
      );
      Export(inst, TSC);
      CurDAG->setRoot(HI.getValue(1));
      return;
    }
    case Type::I128: {
      llvm_unreachable("not implemented");
    }
    case Type::F32: case Type::F64: case Type::F80: {
      llvm_unreachable("not implemented");
    }
  }
}

// -----------------------------------------------------------------------------
void X86ISel::LowerFPUControl(
    unsigned opcode,
    unsigned bytes,
    bool store,
    const Inst *inst)
{
  auto *fpuInst = static_cast<const X86_FPUControlInst *>(inst);

  llvm::MachineMemOperand::Flags flag;
  if (store) {
    flag = llvm::MachineMemOperand::MOStore;
  } else {
    flag = llvm::MachineMemOperand::MOLoad;
  }

  auto *mmo = MF->getMachineMemOperand(
      llvm::MachinePointerInfo(static_cast<llvm::Value *>(nullptr)),
      llvm::MachineMemOperand::MOVolatile | flag,
      bytes,
      llvm::Align(1),
      llvm::AAMDNodes(),
      nullptr,
      llvm::SyncScope::System,
      llvm::AtomicOrdering::SequentiallyConsistent,
      llvm::AtomicOrdering::SequentiallyConsistent
  );

  SDValue addr = GetValue(fpuInst->GetAddr());
  SDValue Ops[] = { CurDAG->getRoot(), addr };
  CurDAG->setRoot(
      CurDAG->getMemIntrinsicNode(
          opcode,
          SDL_,
          CurDAG->getVTList(MVT::Other),
          Ops,
          MVT::i16,
          mmo
      )
  );
}

// -----------------------------------------------------------------------------
void X86ISel::LowerArgs()
{
  for (auto &argLoc : GetX86CallLowering().args()) {
    const llvm::TargetRegisterClass *regClass;
    MVT regType;
    unsigned size;
    switch (argLoc.ArgType) {
      case Type::I8:{
        regType = MVT::i8;
        regClass = &X86::GR8RegClass;
        size = 1;
        break;
      }
      case Type::I16:{
        regType = MVT::i16;
        regClass = &X86::GR16RegClass;
        size = 2;
        break;
      }
      case Type::I32: {
        regType = MVT::i32;
        regClass = &X86::GR32RegClass;
        size = 4;
        break;
      }
      case Type::I64: {
        regType = MVT::i64;
        regClass = &X86::GR64RegClass;
        size = 8;
        break;
      }
      case Type::I128: {
        Error(func_, "Invalid argument to call.");
      }
      case Type::F32: {
        regType = MVT::f32;
        regClass = &X86::FR32RegClass;
        size = 4;
        break;
      }
      case Type::F64: {
        regType = MVT::f64;
        regClass = &X86::FR64RegClass;
        size = 8;
        break;
      }
      case Type::F80: {
        regType = MVT::f80;
        regClass = &X86::RFP80RegClass;
        size = 10;
        break;
      }
    }

    SDValue arg;
    switch (argLoc.Kind) {
      case X86Call::Loc::Kind::REG: {
        unsigned Reg = MF->addLiveIn(argLoc.Reg, regClass);
        arg = CurDAG->getCopyFromReg(CurDAG->getEntryNode(), SDL_, Reg, regType);
        break;
      }
      case X86Call::Loc::Kind::STK: {
        llvm::MachineFrameInfo &MFI = MF->getFrameInfo();
        int index = MFI.CreateFixedObject(size, argLoc.Idx, true);

        args_[argLoc.Index] = index;

        arg = CurDAG->getLoad(
            regType,
            SDL_,
            CurDAG->getEntryNode(),
            CurDAG->getFrameIndex(index, GetPtrTy()),
            llvm::MachinePointerInfo::getFixedStack(
                CurDAG->getMachineFunction(),
                index
            )
        );
        break;
      }
    }

    for (const auto &block : *func_) {
      for (const auto &inst : block) {
        if (!inst.Is(Inst::Kind::ARG)) {
          continue;
        }
        auto &argInst = static_cast<const ArgInst &>(inst);
        if (argInst.GetIdx() == argLoc.Index) {
          Export(&argInst, arg);
        }
      }
    }
  }
}

// -----------------------------------------------------------------------------
void X86ISel::LowerVASetup()
{
  auto &ci = GetX86CallLowering();
  llvm::MachineFrameInfo &MFI = MF->getFrameInfo();
  auto ptrTy = TLI->getPointerTy(CurDAG->getDataLayout());
  SDValue chain = CurDAG->getRoot();

  // Get the size of the stack, plus alignment to store the return
  // address for tail calls for the fast calling convention.
  unsigned stackSize = ci.GetFrameSize();
  switch (func_->GetCallingConv()) {
    case CallingConv::C: {
      break;
    }
    case CallingConv::SETJMP:
    case CallingConv::CAML:
    case CallingConv::CAML_ALLOC:
    case CallingConv::CAML_GC:
    case CallingConv::CAML_RAISE: {
      Error(func_, "vararg call not supported");
    }
  }

  int index = MFI.CreateFixedObject(1, stackSize, false);
  FuncInfo_->setVarArgsFrameIndex(index);

  // Copy all unused regs to be pushed on the stack into vregs.
  llvm::SmallVector<SDValue, 6> liveGPRs;
  llvm::SmallVector<SDValue, 8> liveXMMs;
  SDValue alReg;

  for (unsigned reg : ci.GetUnusedGPRs()) {
    unsigned vreg = MF->addLiveIn(reg, &X86::GR64RegClass);
    liveGPRs.push_back(CurDAG->getCopyFromReg(chain, SDL_, vreg, MVT::i64));
  }

  for (unsigned reg : ci.GetUnusedXMMs()) {
    if (!alReg) {
      unsigned vreg = MF->addLiveIn(X86::AL, &X86::GR8RegClass);
      alReg = CurDAG->getCopyFromReg(chain, SDL_, vreg, MVT::i8);
    }
    unsigned vreg = MF->addLiveIn(reg, &X86::VR128RegClass);
    liveXMMs.push_back(CurDAG->getCopyFromReg(chain, SDL_, vreg, MVT::v4f32));
  }

  // Save the indices to be stored in __va_list_tag
  unsigned numGPRs = ci.GetUnusedGPRs().size() + ci.GetUsedGPRs().size();
  unsigned numXMMs = ci.GetUnusedXMMs().size() + ci.GetUsedXMMs().size();
  FuncInfo_->setVarArgsGPOffset(ci.GetUsedGPRs().size() * 8);
  FuncInfo_->setVarArgsFPOffset(numGPRs * 8 + ci.GetUsedXMMs().size() * 16);
  FuncInfo_->setRegSaveFrameIndex(MFI.CreateStackObject(
      numGPRs * 8 + numXMMs * 16,
      llvm::Align(16),
      false
  ));

  llvm::SmallVector<SDValue, 8> storeOps;
  SDValue frameIdx = CurDAG->getFrameIndex(
      FuncInfo_->getRegSaveFrameIndex(),
      ptrTy
  );

  // Store the unused GPR registers on the stack.
  unsigned gpOffset = FuncInfo_->getVarArgsGPOffset();
  for (SDValue val : liveGPRs) {
    SDValue valIdx = CurDAG->getNode(
        ISD::ADD,
        SDL_,
        ptrTy,
        frameIdx,
        CurDAG->getIntPtrConstant(gpOffset, SDL_)
    );
    storeOps.push_back(CurDAG->getStore(
        val.getValue(1),
        SDL_,
        val,
        valIdx,
        llvm::MachinePointerInfo::getFixedStack(
            CurDAG->getMachineFunction(),
            FuncInfo_->getRegSaveFrameIndex(),
            gpOffset
        )
    ));
    gpOffset += 8;
  }

  // Store the unused XMMs on the stack.
  if (!liveXMMs.empty()) {
    llvm::SmallVector<SDValue, 12> ops;
    ops.push_back(chain);
    ops.push_back(alReg);
    ops.push_back(CurDAG->getIntPtrConstant(
        FuncInfo_->getRegSaveFrameIndex(), SDL_)
    );
    ops.push_back(CurDAG->getIntPtrConstant(
        FuncInfo_->getVarArgsFPOffset(), SDL_)
    );
    ops.insert(ops.end(), liveXMMs.begin(), liveXMMs.end());
    storeOps.push_back(CurDAG->getNode(
        X86ISD::VASTART_SAVE_XMM_REGS,
        SDL_,
        MVT::Other,
        ops
    ));
  }

  if (!storeOps.empty()) {
    chain = CurDAG->getNode(ISD::TokenFactor, SDL_, MVT::Other, storeOps);
  }

  CurDAG->setRoot(chain);
}

// -----------------------------------------------------------------------------
SDValue X86ISel::LowerGetFS()
{
  auto *RegInfo = &MF->getRegInfo();
  const llvm::TargetLowering &TLI = GetTargetLowering();

  // Allocate a register to load FS to.
  auto reg = RegInfo->createVirtualRegister(TLI.getRegClassFor(MVT::i64));

  // Set up the inline assembly node.
  llvm::SmallVector<SDValue, 7> ops;
  ops.push_back(CurDAG->getRoot());
  ops.push_back(CurDAG->getTargetExternalSymbol(
      "mov %fs:0,$0",
      TLI.getProgramPointerTy(CurDAG->getDataLayout())
  ));
  ops.push_back(CurDAG->getMDNode(nullptr));
  ops.push_back(CurDAG->getTargetConstant(
      0,
      SDL_,
      TLI.getPointerTy(CurDAG->getDataLayout())
  ));

  // Register the output.
  {
    unsigned flag = llvm::InlineAsm::getFlagWord(
        llvm::InlineAsm::Kind_RegDef, 1
    );
    const auto *RC = RegInfo->getRegClass(reg);
    flag = llvm::InlineAsm::getFlagWordForRegClass(flag, RC->getID());
    ops.push_back(CurDAG->getTargetConstant(flag, SDL_, MVT::i32));
    ops.push_back(CurDAG->getRegister(reg, MVT::i64));
  }

  // Register clobbers.
  {
    unsigned flag = llvm::InlineAsm::getFlagWord(
        llvm::InlineAsm::Kind_Clobber, 1
    );
    ops.push_back(CurDAG->getTargetConstant(flag, SDL_, MVT::i32));
    ops.push_back(CurDAG->getRegister(X86::DF, MVT::i32));
    ops.push_back(CurDAG->getTargetConstant(flag, SDL_, MVT::i32));
    ops.push_back(CurDAG->getRegister(X86::FPSW, MVT::i16));
    ops.push_back(CurDAG->getTargetConstant(flag, SDL_, MVT::i32));
    ops.push_back(CurDAG->getRegister(X86::EFLAGS, MVT::i32));
  }

  // Create the inlineasm node.
  SDValue node = CurDAG->getNode(
      ISD::INLINEASM,
      SDL_,
      CurDAG->getVTList(MVT::Other, MVT::Glue),
      ops
  );
  SDValue chain = node.getValue(0);
  SDValue glue = node.getValue(1);

  // Copy out the vreg.
  auto copy = CurDAG->getCopyFromReg(
      chain,
      SDL_,
      reg,
      MVT::i64,
      glue
  );
  CurDAG->setRoot(copy.getValue(1));
  return copy.getValue(0);
}

// -----------------------------------------------------------------------------
void X86ISel::LowerSetSP(SDValue value)
{
  CurDAG->setRoot(CurDAG->getCopyToReg(
      CurDAG->getRoot(),
      SDL_,
      X86::RSP,
      value
  ));
}

// -----------------------------------------------------------------------------
SDValue X86ISel::LoadReg(ConstantReg::Kind reg)
{
  switch (reg) {
    // Thread pointer.
    case ConstantReg::Kind::FS: {
      return LowerGetFS();
    }
    // Program counter.
    case ConstantReg::Kind::PC: {
      auto &MMI = MF->getMMI();
      auto *label = MMI.getContext().createTempSymbol();
      CurDAG->setRoot(CurDAG->getEHLabel(SDL_, CurDAG->getRoot(), label));
      return CurDAG->getNode(
          X86ISD::WrapperRIP,
          SDL_,
          MVT::i64,
          CurDAG->getMCSymbol(label, MVT::i64)
      );
    }
    // Stack pointer.
    case ConstantReg::Kind::SP: {
      return CurDAG->getNode(ISD::STACKSAVE, SDL_, MVT::i64, CurDAG->getRoot());
    }
    // Return address.
    case ConstantReg::Kind::RET_ADDR: {
      return CurDAG->getNode(
          ISD::RETURNADDR,
          SDL_,
          MVT::i64,
          CurDAG->getTargetConstant(0, SDL_, MVT::i64)
      );
    }
    // Frame address.
    case ConstantReg::Kind::FRAME_ADDR: {
      MF->getFrameInfo().setReturnAddressIsTaken(true);

      if (frameIndex_ == 0) {
        frameIndex_ = MF->getFrameInfo().CreateFixedObject(8, 0, false);
      }

      return CurDAG->getFrameIndex(frameIndex_, MVT::i64);
    }
  }
  llvm_unreachable("invalid register kind");
}

// -----------------------------------------------------------------------------
llvm::SDValue X86ISel::LowerGlobal(const Global *val, int64_t offset)
{
  if (offset == 0) {
    return LowerGlobal(val);
  } else {
    return CurDAG->getNode(
        ISD::ADD,
        SDL_,
        GetPtrTy(),
        LowerGlobal(val),
        CurDAG->getConstant(offset, SDL_, GetPtrTy())
    );
  }
}

// -----------------------------------------------------------------------------
llvm::SDValue X86ISel::LowerGlobal(const Global *val)
{
  const std::string_view name = val->GetName();
  const auto ptrTy = GetPtrTy();

  switch (val->GetKind()) {
    case Global::Kind::BLOCK: {
      auto *block = static_cast<const Block *>(val);
      if (auto *MBB = blocks_[block]) {
        auto *BB = const_cast<llvm::BasicBlock *>(MBB->getBasicBlock());
        auto *BA = llvm::BlockAddress::get(F_, BB);
        return CurDAG->getBlockAddress(BA, ptrTy);
      } else {
        llvm::report_fatal_error("Unknown block '" + std::string(name) + "'");
      }
    }
    case Global::Kind::FUNC:
    case Global::Kind::ATOM: {
      // Atom reference - need indirection for shared objects.
      auto *GV = M_->getNamedValue(name.data());
      if (!GV) {
        llvm::report_fatal_error("Unknown symbol '" + std::string(name) + "'");
        break;
      }

      if (shared_ && !val->IsLocal()) {
        SDValue addr = CurDAG->getTargetGlobalAddress(
            GV,
            SDL_,
            ptrTy,
            0,
            llvm::X86II::MO_GOTPCREL
        );

        SDValue addrRIP = CurDAG->getNode(
            X86ISD::WrapperRIP,
            SDL_,
            ptrTy,
            addr
        );

        return CurDAG->getLoad(
            ptrTy,
            SDL_,
            CurDAG->getEntryNode(),
            addrRIP,
            llvm::MachinePointerInfo::getGOT(CurDAG->getMachineFunction())
        );
      } else {
        return CurDAG->getNode(
            X86ISD::WrapperRIP,
            SDL_,
            ptrTy, CurDAG->getTargetGlobalAddress(
                GV,
                SDL_,
                ptrTy,
                0,
                llvm::X86II::MO_NO_FLAG
            )
        );
      }
    }
    case Global::Kind::EXTERN: {
      // Extern reference.
      auto *GV = M_->getNamedValue(name.data());
      if (!GV) {
        llvm::report_fatal_error("Unknown extern '" + std::string(name) + "'");
      }

      auto *ext = static_cast<const Extern *>(val);
      if (ext->GetSection() == ".text") {
        // Text-to-text references can be RIP-relative.
        return CurDAG->getNode(
            X86ISD::WrapperRIP,
            SDL_,
            ptrTy, CurDAG->getTargetGlobalAddress(
                GV,
                SDL_,
                ptrTy,
                0,
                llvm::X86II::MO_NO_FLAG
            )
        );
      } else {
         SDValue addr = CurDAG->getTargetGlobalAddress(
            GV,
            SDL_,
            ptrTy,
            0,
            llvm::X86II::MO_GOTPCREL
        );

        SDValue addrRIP = CurDAG->getNode(
            X86ISD::WrapperRIP,
            SDL_,
            ptrTy,
            addr
        );

        return CurDAG->getLoad(
            ptrTy,
            SDL_,
            CurDAG->getEntryNode(),
            addrRIP,
            llvm::MachinePointerInfo::getGOT(CurDAG->getMachineFunction())
        );
      }
    }
  }
  llvm_unreachable("invalid global type");
}

// -----------------------------------------------------------------------------
llvm::SDValue X86ISel::LowerCallee(const Inst *inst)
{
  if (auto *movInst = ::dyn_cast_or_null<const MovInst>(inst)) {
    auto *movArg = GetMoveArg(movInst);
    switch (movArg->GetKind()) {
      case Value::Kind::INST: {
        return GetValue(static_cast<const Inst *>(movArg));
        break;
      }
      case Value::Kind::GLOBAL: {
        auto *movGlobal = static_cast<const Global *>(movArg);
        switch (movGlobal->GetKind()) {
          case Global::Kind::BLOCK: {
            llvm_unreachable("invalid call argument");
          }
          case Global::Kind::FUNC:
          case Global::Kind::ATOM:
          case Global::Kind::EXTERN: {
            const std::string_view name = movGlobal->GetName();
            if (auto *GV = M_->getNamedValue(name.data())) {
              return CurDAG->getTargetGlobalAddress(
                  GV,
                  SDL_,
                  MVT::i64,
                  0,
                  llvm::X86II::MO_NO_FLAG
              );
            } else {
              Error(inst, "Unknown symbol '" + std::string(name) + "'");
            }
            break;
          }
        }
        llvm_unreachable("invalid global kind");
      }
      case Value::Kind::EXPR:
      case Value::Kind::CONST: {
        llvm_unreachable("invalid call argument");
      }
    }
    llvm_unreachable("invalid value kind");
  } else {
    return GetValue(inst);
  }
}

// -----------------------------------------------------------------------------
llvm::ScheduleDAGSDNodes *X86ISel::CreateScheduler()
{
  return createILPListDAGScheduler(MF, TII, TRI_, TLI, OptLevel);
}

// -----------------------------------------------------------------------------
void X86ISel::LowerCallSite(SDValue chain, const CallSite *call)
{
  const Block *block = call->getParent();
  const Func *func = block->getParent();
  auto ptrTy = TLI->getPointerTy(CurDAG->getDataLayout());
  auto &MMI = getAnalysis<llvm::MachineModuleInfoWrapperPass>().getMMI();

  // Analyse the arguments, finding registers for them.
  bool isVarArg = call->GetNumArgs() > call->GetNumFixedArgs();
  bool isTailCall = call->Is(Inst::Kind::TCALL);
  bool isInvoke = call->Is(Inst::Kind::INVOKE);
  bool wasTailCall = isTailCall;
  X86Call locs(call);

  // Find the number of bytes allocated to hold arguments.
  unsigned stackSize = locs.GetFrameSize();

  // Compute the stack difference for tail calls.
  int fpDiff = 0;
  if (isTailCall) {
    X86Call callee(func);
    int bytesToPop;
    switch (func->GetCallingConv()) {
      case CallingConv::C: {
        if (func->IsVarArg()) {
          bytesToPop = callee.GetFrameSize();
        } else {
          bytesToPop = 0;
        }
        break;
      }
      case CallingConv::SETJMP:
      case CallingConv::CAML:
      case CallingConv::CAML_ALLOC:
      case CallingConv::CAML_GC:
      case CallingConv::CAML_RAISE: {
        bytesToPop = 0;
        break;
      }
    }
    fpDiff = bytesToPop - static_cast<int>(stackSize);
  }

  if (isTailCall && fpDiff) {
    // TODO: some tail calls can still be lowered.
    wasTailCall = true;
    isTailCall = false;
  }

  // Determine whether the call site need an OCaml frame.
  const bool hasFrame = call->template HasAnnot<CamlFrame>();

  // Calls from OCaml to C need to go through a trampoline.
  bool needsTrampoline = false;
  if (func->GetCallingConv() == CallingConv::CAML) {
    switch (call->GetCallingConv()) {
      case CallingConv::C:
        needsTrampoline = hasFrame;
        break;
      case CallingConv::SETJMP:
      case CallingConv::CAML:
      case CallingConv::CAML_ALLOC:
      case CallingConv::CAML_GC:
      case CallingConv::CAML_RAISE:
        break;
    }
  }

  // Find the register mask, based on the calling convention.
  unsigned cc;
  {
    using namespace llvm::CallingConv;
    if (needsTrampoline) {
      cc = LLIR_CAML_EXT;
    } else {
      switch (call->GetCallingConv()) {
        case CallingConv::C:          cc = C;               break;
        case CallingConv::CAML:       cc = LLIR_CAML;       break;
        case CallingConv::CAML_ALLOC: cc = LLIR_CAML_ALLOC; break;
        case CallingConv::CAML_GC:    cc = LLIR_CAML_GC;    break;
        case CallingConv::CAML_RAISE: cc = LLIR_CAML_RAISE; break;
        case CallingConv::SETJMP:     cc = LLIR_SETJMP;     break;
      }
    }
  }
  const uint32_t *mask = TRI_->getCallPreservedMask(*MF, cc);

  // Instruction bundle starting the call.
  chain = CurDAG->getCALLSEQ_START(chain, stackSize, 0, SDL_);

  // Identify registers and stack locations holding the arguments.
  llvm::SmallVector<std::pair<unsigned, SDValue>, 8> regArgs;
  llvm::SmallVector<SDValue, 8> memOps;
  SDValue stackPtr;
  for (auto it = locs.arg_begin(); it != locs.arg_end(); ++it) {
    SDValue argument = GetValue(it->Value);
    switch (it->Kind) {
      case CallLowering::Loc::Kind::REG: {
        regArgs.emplace_back(it->Reg, argument);
        break;
      }
      case CallLowering::Loc::Kind::STK: {
        if (!stackPtr.getNode()) {
          stackPtr = CurDAG->getCopyFromReg(
              chain,
              SDL_,
              TRI_->getStackRegister(),
              ptrTy
          );
        }

        SDValue memOff = CurDAG->getNode(
            ISD::ADD,
            SDL_,
            ptrTy,
            stackPtr,
            CurDAG->getIntPtrConstant(it->Idx, SDL_)
        );

        memOps.push_back(CurDAG->getStore(
            chain,
            SDL_,
            argument,
            memOff,
            llvm::MachinePointerInfo::getStack(*MF, it->Idx)
        ));

        break;
      }
    }
  }

  if (!memOps.empty()) {
    chain = CurDAG->getNode(ISD::TokenFactor, SDL_, MVT::Other, memOps);
  }

  if (isVarArg) {
    // If XMM regs are used, their count needs to be passed in AL.
    unsigned count = 0;
    for (auto arg : call->args()) {
      if (IsFloatType(arg->GetType(0))) {
        count = std::min(8u, count + 1);
      }
    }

    regArgs.push_back({ X86::AL, CurDAG->getConstant(count, SDL_, MVT::i8) });
  }

  if (isTailCall) {
    // Shuffle arguments on the stack.
    for (auto it = locs.arg_begin(); it != locs.arg_end(); ++it) {
      switch (it->Kind) {
        case X86Call::Loc::Kind::REG: {
          continue;
        }
        case X86Call::Loc::Kind::STK: {
          llvm_unreachable("not implemented");
          break;
        }
      }
    }

    // Store the return address.
    if (fpDiff) {
      llvm_unreachable("not implemented");
    }
  }

  // Find the callee.
  SDValue callee;
  if (needsTrampoline) {
    // If call goes through a trampoline, replace the callee
    // and add the original one as the argument passed through $rax.
    if (!trampoline_) {
      trampoline_ = llvm::Function::Create(
          funcTy_,
          GlobalValue::ExternalLinkage,
          0,
          "caml_c_call",
          M_
      );
    }
    regArgs.emplace_back(X86::RAX, GetValue(call->GetCallee()));
    callee = CurDAG->getTargetGlobalAddress(
        trampoline_,
        SDL_,
        MVT::i64,
        0,
        llvm::X86II::MO_NO_FLAG
    );
  } else {
    callee = LowerCallee(call->GetCallee());
  }

  // Prepare arguments in registers.
  SDValue inFlag;
  for (const auto &reg : regArgs) {
    chain = CurDAG->getCopyToReg(
        chain,
        SDL_,
        reg.first,
        reg.second,
        inFlag
    );
    inFlag = chain.getValue(1);
  }

  // Finish the call here for tail calls.
  if (isTailCall) {
    chain = CurDAG->getCALLSEQ_END(
        chain,
        CurDAG->getIntPtrConstant(stackSize, SDL_, true),
        CurDAG->getIntPtrConstant(0, SDL_, true),
        inFlag,
        SDL_
    );
    inFlag = chain.getValue(1);
  }

  // Create the DAG node for the Call.
  llvm::SmallVector<SDValue, 8> ops;
  ops.push_back(chain);
  ops.push_back(callee);
  if (isTailCall) {
    ops.push_back(CurDAG->getConstant(fpDiff, SDL_, MVT::i32));
  }
  for (const auto &reg : regArgs) {
    ops.push_back(CurDAG->getRegister(
        reg.first,
        reg.second.getValueType()
    ));
  }
  ops.push_back(CurDAG->getRegisterMask(mask));

  // Finalize the call node.
  if (inFlag.getNode()) {
    ops.push_back(inFlag);
  }

  // Generate a call or a tail call.
  SDVTList nodeTypes = CurDAG->getVTList(MVT::Other, MVT::Glue);
  if (isTailCall) {
    MF->getFrameInfo().setHasTailCall();
    CurDAG->setRoot(CurDAG->getNode(X86ISD::TC_RETURN, SDL_, nodeTypes, ops));
  } else {
    chain = CurDAG->getNode(X86ISD::CALL, SDL_, nodeTypes, ops);
    inFlag = chain.getValue(1);

    // Find the register to store the return value in.
    std::optional<std::pair<llvm::Register, MVT>> retReg;
    if (auto retTy = call->GetType()) {
      switch (*retTy) {
        case Type::I8: retReg = { X86::AL, MVT::i8 }; break;
        case Type::I16: retReg = { X86::AX, MVT::i16 }; break;
        case Type::I32: retReg = { X86::EAX, MVT::i32 }; break;
        case Type::I64: retReg = { X86::RAX, MVT::i64 }; break;
        case Type::F32: retReg = { X86::XMM0, MVT::f32 }; break;
        case Type::F64: retReg = { X86::XMM0, MVT::f64 }; break;
        case Type::F80: retReg = { X86::FP0, MVT::f80 }; break;
        case Type::I128: Error(call, "unsupported return value type");
      }
    }

    // Generate a GC_FRAME before the call, if needed.
    if (hasFrame && !isTailCall) {
      if ((wasTailCall || !call->use_empty()) && retReg) {
        chain = LowerGCFrame(chain, inFlag, call, mask, retReg->first);
      } else {
        chain = LowerGCFrame(chain, inFlag, call, mask, {});
      }
      inFlag = chain.getValue(1);
    }

    chain = CurDAG->getCALLSEQ_END(
        chain,
        CurDAG->getIntPtrConstant(stackSize, SDL_, true),
        CurDAG->getIntPtrConstant(0, SDL_, true),
        inFlag,
        SDL_
    );

    // Lower the return value.
    std::vector<SDValue> tailReturns;
    if (retReg) {
      // Find the physical reg where the return value is stored.
      if (wasTailCall || !isTailCall) {
        if (wasTailCall) {
          /// Copy the return value into a vreg.
          chain = CurDAG->getCopyFromReg(
              chain,
              SDL_,
              retReg->first,
              retReg->second,
              chain.getValue(1)
          ).getValue(1);

          /// If this was a tailcall, forward to return.
          tailReturns.push_back(chain.getValue(0));
        } else {
          // Regular call with a return which is used - expose it.
          if (!call->use_empty()) {
            chain = CurDAG->getCopyFromReg(
                chain,
                SDL_,
                retReg->first,
                retReg->second,
                chain.getValue(1)
            ).getValue(1);

            // Otherwise, expose the value.
            Export(call, chain.getValue(0));
          }
        }
      }
    }

    if (wasTailCall) {
      llvm::SmallVector<SDValue, 6> returns;
      returns.push_back(chain);
      returns.push_back(CurDAG->getTargetConstant(0, SDL_, MVT::i32));
      for (auto &ret : tailReturns) {
        returns.push_back(ret);
      }

      chain = CurDAG->getNode(
          X86ISD::RET_FLAG,
          SDL_,
          MVT::Other,
          returns
      );
    }

    CurDAG->setRoot(chain);
  }
}

// -----------------------------------------------------------------------------
void X86ISel::LowerSyscall(const SyscallInst *inst)
{
  static unsigned kRegs[] = {
      X86::RDI, X86::RSI, X86::RDX,
      X86::R10, X86::R8, X86::R9
  };

  llvm::SmallVector<SDValue, 7> ops;
  SDValue chain = CurDAG->getRoot();

  // Lower arguments.
  unsigned args = 0;
  {
    unsigned n = sizeof(kRegs) / sizeof(kRegs[0]);
    for (const Inst *arg : inst->args()) {
      if (args >= n) {
        Error(inst, "too many arguments to syscall");
      }

      SDValue value = GetValue(arg);
      if (arg->GetType(0) != Type::I64) {
        Error(inst, "invalid syscall argument");
      }
      ops.push_back(CurDAG->getRegister(kRegs[args], MVT::i64));
      chain = CurDAG->getCopyToReg(chain, SDL_, kRegs[args++], value);
    }
  }

  /// Lower to the syscall.
  {
    ops.push_back(CurDAG->getRegister(X86::RAX, MVT::i64));

    chain = CurDAG->getCopyToReg(
        chain,
        SDL_,
        X86::RAX,
        GetValue(inst->GetSyscall())
    );

    ops.push_back(chain);

    chain = SDValue(CurDAG->getMachineNode(
        X86::SYSCALL,
        SDL_,
        CurDAG->getVTList(MVT::Other, MVT::Glue),
        ops
    ), 0);
  }

  /// Copy the return value into a vreg and export it.
  {
    if (auto type = inst->GetType()) {
      if (*type != Type::I64) {
        Error(inst, "invalid syscall type");
      }

      chain = CurDAG->getCopyFromReg(
          chain,
          SDL_,
          X86::RAX,
          MVT::i64,
          chain.getValue(1)
      ).getValue(1);

      Export(inst, chain.getValue(0));
    }
  }

  CurDAG->setRoot(chain);
}

// -----------------------------------------------------------------------------
void X86ISel::LowerClone(const CloneInst *inst)
{
  auto *RegInfo = &MF->getRegInfo();
  const llvm::TargetLowering &TLI = GetTargetLowering();

  // Copy in the new stack pointer and code pointer.
  SDValue chain;
  unsigned callee = RegInfo->createVirtualRegister(TLI.getRegClassFor(MVT::i64));
  chain = CurDAG->getCopyToReg(
      CurDAG->getRoot(),
      SDL_,
      callee,
      GetValue(inst->GetCallee()),
      chain
  );
  unsigned arg = RegInfo->createVirtualRegister(TLI.getRegClassFor(MVT::i64));
  chain = CurDAG->getCopyToReg(
      CurDAG->getRoot(),
      SDL_,
      arg,
      GetValue(inst->GetArg()),
      chain
  );

  // Copy in other registers.
  auto CopyReg = [&](const Inst *arg, unsigned reg) {
    chain = CurDAG->getCopyToReg(
        CurDAG->getRoot(),
        SDL_,
        reg,
        GetValue(arg),
        chain
    );
  };

  CopyReg(inst->GetFlags(), X86::RDI);
  CopyReg(inst->GetStack(), X86::RSI);
  CopyReg(inst->GetPTID(), X86::RDX);
  CopyReg(inst->GetCTID(), X86::R10);
  CopyReg(inst->GetTLS(), X86::R8);

  // Set up the inline assembly node.
  llvm::SmallVector<SDValue, 7> ops;
  ops.push_back(chain);
  ops.push_back(CurDAG->getTargetExternalSymbol(
      "and $$-16, %rsi;"
      "sub $$16, %rsi;"
      "mov $2, (%rsi);"
      "mov $1, 8(%rsi);"
      "mov $$56, %eax;"
      "syscall;"
      "test %eax, %eax;"
      "jnz 1f;"
      "xor %ebp, %ebp;"
      "pop %rdi;"
      "pop %r9;"
      "call *%r9;"
      "mov %eax, %edi;"
      "mov $$60, %eax;"
      "syscall;"
      "hlt;"
      "1:",
      TLI.getProgramPointerTy(CurDAG->getDataLayout())
  ));
  ops.push_back(CurDAG->getMDNode(nullptr));
  ops.push_back(CurDAG->getTargetConstant(
      llvm::InlineAsm::Extra_MayLoad | llvm::InlineAsm::Extra_MayStore,
      SDL_,
      TLI.getPointerTy(CurDAG->getDataLayout())
  ));

  // Register the output.
  {
    unsigned flag = llvm::InlineAsm::getFlagWord(
        llvm::InlineAsm::Kind_RegDef, 1
    );
    ops.push_back(CurDAG->getTargetConstant(flag, SDL_, MVT::i32));
    ops.push_back(CurDAG->getRegister(X86::RAX, MVT::i64));
  }

  // Register the input.
  {
    auto AddVirtReg = [&] (unsigned reg) {
      const auto *RC = RegInfo->getRegClass(reg);
      const unsigned flag = llvm::InlineAsm::getFlagWordForRegClass(
          llvm::InlineAsm::getFlagWord(llvm::InlineAsm::Kind_RegUse, 1),
          RC->getID()
      );
      ops.push_back(CurDAG->getTargetConstant(flag, SDL_, MVT::i32));
      ops.push_back(CurDAG->getRegister(reg, MVT::i64));
    };
    auto AddPhysReg = [&] (unsigned reg) {
      const unsigned flag = llvm::InlineAsm::getFlagWord(
          llvm::InlineAsm::Kind_RegUse, 1
      );
      ops.push_back(CurDAG->getTargetConstant(flag, SDL_, MVT::i32));
      ops.push_back(CurDAG->getRegister(reg, MVT::i64));
    };
    AddVirtReg(callee);   // 1
    AddVirtReg(arg);      // 2
    AddPhysReg(X86::RSI); // 3
    AddPhysReg(X86::RDI); // 4
    AddPhysReg(X86::RDX); // 5
    AddPhysReg(X86::R10); // 6
    AddPhysReg(X86::R8);  // 7
  }

  // Add the chain.
  ops.push_back(chain.getValue(1));

  // Create the inlineasm node.
  chain = CurDAG->getNode(
      ISD::INLINEASM,
      SDL_,
      CurDAG->getVTList(MVT::Other, MVT::Glue),
      ops
  );

  /// Copy the return value into a vreg and export it.
  {
    if (inst->GetType() != Type::I64) {
      Error(inst, "invalid clone type");
    }

    chain = CurDAG->getCopyFromReg(
        chain,
        SDL_,
        X86::RAX,
        MVT::i64,
        chain.getValue(1)
    ).getValue(1);

    Export(inst, chain.getValue(0));
  }

  // Update the root.
  CurDAG->setRoot(chain);
}

// -----------------------------------------------------------------------------
void X86ISel::LowerSwitch(const SwitchInst *inst)
{
  llvm::SelectionDAG &dag = GetDAG();
  llvm::MachineFunction &MF = dag.getMachineFunction();
  const llvm::TargetLowering &TLI = GetTargetLowering();

  auto *sourceMBB = blocks_[inst->getParent()];

  std::vector<llvm::MachineBasicBlock*> branches;
  for (unsigned i = 0, n = inst->getNumSuccessors(); i < n; ++i) {
    auto *mbb = blocks_[inst->getSuccessor(i)];
    branches.push_back(mbb);
  }

  {
    llvm::DenseSet<llvm::MachineBasicBlock *> added;
    for (auto *mbb : branches) {
      if (added.insert(mbb).second) {
        sourceMBB->addSuccessor(mbb);
      }
    }
  }

  sourceMBB->normalizeSuccProbs();

  auto *jti = MF.getOrCreateJumpTableInfo(TLI.getJumpTableEncoding());
  int jumpTableId = jti->createJumpTableIndex(branches);
  auto ptrTy = TLI.getPointerTy(dag.getDataLayout());

  SDValue jt = dag.getTargetJumpTable(
      jumpTableId,
      ptrTy,
      llvm::X86II::MO_NO_FLAG
  );

  jt = dag.getNode(
      X86ISD::WrapperRIP,
      SDL_,
      ptrTy,
      jt
  );

  dag.setRoot(dag.getNode(
      ISD::BR_JT,
      SDL_,
      MVT::Other,
      GetExportRoot(),
      jt,
      GetValue(inst->GetIdx())
  ));
}

// -----------------------------------------------------------------------------
void X86ISel::LowerRaise(const RaiseInst *inst)
{
  return LowerRaise(
      GetValue(inst->GetStack()),
      GetValue(inst->GetTarget()),
      SDValue()
  );
}

// -----------------------------------------------------------------------------
void X86ISel::LowerReturnJump(const ReturnJumpInst *inst)
{
  return LowerRaise(
      GetValue(inst->GetStack()),
      GetValue(inst->GetTarget()),
      CurDAG->getCopyToReg(
          CurDAG->getRoot(),
          SDL_,
          X86::RAX,
          GetValue(inst->GetValue()),
          SDValue()
      )
  );
}

// -----------------------------------------------------------------------------
void X86ISel::LowerRaise(SDValue spVal, SDValue pcVal, SDValue glue)
{
  auto *RegInfo = &MF->getRegInfo();
  const llvm::TargetLowering &TLI = GetTargetLowering();

  // Allocate a register to load FS to.
  auto pc = RegInfo->createVirtualRegister(TLI.getRegClassFor(MVT::i64));
  auto stk = RegInfo->createVirtualRegister(TLI.getRegClassFor(MVT::i64));

  // Copy in the new stack pointer and code pointer.
  SDValue stkNode = CurDAG->getCopyToReg(
      CurDAG->getRoot(),
      SDL_,
      stk,
      spVal,
      glue
  );
  SDValue pcNode = CurDAG->getCopyToReg(
      stkNode,
      SDL_,
      pc,
      pcVal,
      stkNode
  );

  // Set up the inline assembly node.
  llvm::SmallVector<SDValue, 7> ops;
  ops.push_back(pcNode);
  ops.push_back(CurDAG->getTargetExternalSymbol(
      "mov $0, %rsp ; jmp *$1",
      TLI.getProgramPointerTy(CurDAG->getDataLayout())
  ));
  ops.push_back(CurDAG->getMDNode(nullptr));
  ops.push_back(CurDAG->getTargetConstant(
      llvm::InlineAsm::Extra_MayLoad | llvm::InlineAsm::Extra_MayStore,
      SDL_,
      TLI.getPointerTy(CurDAG->getDataLayout())
  ));

  // Register the input.
  {
    auto AddRegister = [&] (unsigned reg) {
      const auto *RC = RegInfo->getRegClass(reg);
      const unsigned flag = llvm::InlineAsm::getFlagWordForRegClass(
          llvm::InlineAsm::getFlagWord(llvm::InlineAsm::Kind_RegUse, 1),
          RC->getID()
      );
      ops.push_back(CurDAG->getTargetConstant(flag, SDL_, MVT::i32));
      ops.push_back(CurDAG->getRegister(reg, MVT::i64));
    };
    AddRegister(stk);
    AddRegister(pc);
    if (glue) {
      const unsigned flag = llvm::InlineAsm::getFlagWord(
          llvm::InlineAsm::Kind_RegUse, 1
      );
      ops.push_back(CurDAG->getTargetConstant(flag, SDL_, MVT::i32));
      ops.push_back(CurDAG->getRegister(X86::RAX, MVT::i32));
    }
  }

  // Register clobbers.
  {
    unsigned flag = llvm::InlineAsm::getFlagWord(
        llvm::InlineAsm::Kind_Clobber, 1
    );
    ops.push_back(CurDAG->getTargetConstant(flag, SDL_, MVT::i32));
    ops.push_back(CurDAG->getRegister(X86::DF, MVT::i32));
    ops.push_back(CurDAG->getTargetConstant(flag, SDL_, MVT::i32));
    ops.push_back(CurDAG->getRegister(X86::FPSW, MVT::i16));
    ops.push_back(CurDAG->getTargetConstant(flag, SDL_, MVT::i32));
    ops.push_back(CurDAG->getRegister(X86::EFLAGS, MVT::i32));
  }

  // Add the glue.
  ops.push_back(pcNode.getValue(1));

  // Create the inlineasm node.
  SDValue node = CurDAG->getNode(
      ISD::INLINEASM,
      SDL_,
      CurDAG->getVTList(MVT::Other, MVT::Glue),
      ops
  );
  CurDAG->setRoot(node);
}

// -----------------------------------------------------------------------------
void X86ISel::LowerXchg(const X86_XchgInst *inst)
{
  llvm::SelectionDAG &dag = GetDAG();

  auto *mmo = dag.getMachineFunction().getMachineMemOperand(
      llvm::MachinePointerInfo(static_cast<llvm::Value *>(nullptr)),
      llvm::MachineMemOperand::MOVolatile |
      llvm::MachineMemOperand::MOLoad |
      llvm::MachineMemOperand::MOStore,
      GetSize(inst->GetType()),
      llvm::Align(GetSize(inst->GetType())),
      llvm::AAMDNodes(),
      nullptr,
      llvm::SyncScope::System,
      llvm::AtomicOrdering::SequentiallyConsistent,
      llvm::AtomicOrdering::SequentiallyConsistent
  );

  SDValue xchg = dag.getAtomic(
      ISD::ATOMIC_SWAP,
      SDL_,
      GetType(inst->GetType()),
      dag.getRoot(),
      GetValue(inst->GetAddr()),
      GetValue(inst->GetVal()),
      mmo
  );

  dag.setRoot(xchg.getValue(1));
  Export(inst, xchg.getValue(0));
}

// -----------------------------------------------------------------------------
X86Call &X86ISel::GetX86CallLowering()
{
  if (!conv_ || conv_->first != func_) {
    conv_ = std::make_unique<std::pair<const Func *, X86Call>>(
        func_,
        X86Call{ func_ }
    );
  }
  return conv_->second;
}
