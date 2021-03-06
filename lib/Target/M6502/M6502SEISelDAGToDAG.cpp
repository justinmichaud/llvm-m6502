//===-- M6502SEISelDAGToDAG.cpp - A Dag to Dag Inst Selector for M6502SE ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Subclass of M6502DAGToDAGISel specialized for m650232/64.
//
//===----------------------------------------------------------------------===//

#include "M6502SEISelDAGToDAG.h"
#include "MCTargetDesc/M6502BaseInfo.h"
#include "M6502.h"
#include "M6502AnalyzeImmediate.h"
#include "M6502MachineFunction.h"
#include "M6502RegisterInfo.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAGNodes.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
using namespace llvm;

#define DEBUG_TYPE "m6502-isel"

bool M6502SEDAGToDAGISel::runOnMachineFunction(MachineFunction &MF) {
  Subtarget = &static_cast<const M6502Subtarget &>(MF.getSubtarget());
  if (Subtarget->inM650216Mode())
    return false;
  return M6502DAGToDAGISel::runOnMachineFunction(MF);
}

void M6502SEDAGToDAGISel::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DominatorTreeWrapperPass>();
  SelectionDAGISel::getAnalysisUsage(AU);
}

void M6502SEDAGToDAGISel::addDSPCtrlRegOperands(bool IsDef, MachineInstr &MI,
                                               MachineFunction &MF) {
  MachineInstrBuilder MIB(MF, &MI);
  unsigned Mask = MI.getOperand(1).getImm();
  unsigned Flag =
      IsDef ? RegState::ImplicitDefine : RegState::Implicit | RegState::Undef;

  if (Mask & 1)
    MIB.addReg(M6502::DSPPos, Flag);

  if (Mask & 2)
    MIB.addReg(M6502::DSPSCount, Flag);

  if (Mask & 4)
    MIB.addReg(M6502::DSPCarry, Flag);

  if (Mask & 8)
    MIB.addReg(M6502::DSPOutFlag, Flag);

  if (Mask & 16)
    MIB.addReg(M6502::DSPCCond, Flag);

  if (Mask & 32)
    MIB.addReg(M6502::DSPEFI, Flag);
}

unsigned M6502SEDAGToDAGISel::getMSACtrlReg(const SDValue RegIdx) const {
  switch (cast<ConstantSDNode>(RegIdx)->getZExtValue()) {
  default:
    llvm_unreachable("Could not map int to register");
  case 0: return M6502::MSAIR;
  case 1: return M6502::MSACSR;
  case 2: return M6502::MSAAccess;
  case 3: return M6502::MSASave;
  case 4: return M6502::MSAModify;
  case 5: return M6502::MSARequest;
  case 6: return M6502::MSAMap;
  case 7: return M6502::MSAUnmap;
  }
}

bool M6502SEDAGToDAGISel::replaceUsesWithZeroReg(MachineRegisterInfo *MRI,
                                                const MachineInstr& MI) {
  unsigned DstReg = 0, ZeroReg = 0;

  // Check if MI is "addiu $dst, $zero, 0" or "daddiu $dst, $zero, 0".
  if ((MI.getOpcode() == M6502::ADDiu) &&
      (MI.getOperand(1).getReg() == M6502::ZERO) &&
      (MI.getOperand(2).isImm()) &&
      (MI.getOperand(2).getImm() == 0)) {
    DstReg = MI.getOperand(0).getReg();
    ZeroReg = M6502::ZERO;
  } else if ((MI.getOpcode() == M6502::DADDiu) &&
             (MI.getOperand(1).getReg() == M6502::ZERO_64) &&
             (MI.getOperand(2).isImm()) &&
             (MI.getOperand(2).getImm() == 0)) {
    DstReg = MI.getOperand(0).getReg();
    ZeroReg = M6502::ZERO_64;
  }

  if (!DstReg)
    return false;

  // Replace uses with ZeroReg.
  for (MachineRegisterInfo::use_iterator U = MRI->use_begin(DstReg),
       E = MRI->use_end(); U != E;) {
    MachineOperand &MO = *U;
    unsigned OpNo = U.getOperandNo();
    MachineInstr *MI = MO.getParent();
    ++U;

    // Do not replace if it is a phi's operand or is tied to def operand.
    if (MI->isPHI() || MI->isRegTiedToDefOperand(OpNo) || MI->isPseudo())
      continue;

    // Also, we have to check that the register class of the operand
    // contains the zero register.
    if (!MRI->getRegClass(MO.getReg())->contains(ZeroReg))
      continue;

    MO.setReg(ZeroReg);
  }

  return true;
}

void M6502SEDAGToDAGISel::initGlobalBaseReg(MachineFunction &MF) {
  M6502FunctionInfo *M6502FI = MF.getInfo<M6502FunctionInfo>();

  if (!M6502FI->globalBaseRegSet())
    return;

  MachineBasicBlock &MBB = MF.front();
  MachineBasicBlock::iterator I = MBB.begin();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();
  const TargetInstrInfo &TII = *Subtarget->getInstrInfo();
  DebugLoc DL;
  unsigned V0, V1, GlobalBaseReg = M6502FI->getGlobalBaseReg();
  const TargetRegisterClass *RC;
  const M6502ABIInfo &ABI = static_cast<const M6502TargetMachine &>(TM).getABI();
  RC = (ABI.IsN64()) ? &M6502::GPR64RegClass : &M6502::GPR32RegClass;

  V0 = RegInfo.createVirtualRegister(RC);
  V1 = RegInfo.createVirtualRegister(RC);

  if (ABI.IsN64()) {
    MF.getRegInfo().addLiveIn(M6502::T9_64);
    MBB.addLiveIn(M6502::T9_64);

    // lui $v0, %hi(%neg(%gp_rel(fname)))
    // daddu $v1, $v0, $t9
    // daddiu $globalbasereg, $v1, %lo(%neg(%gp_rel(fname)))
    const GlobalValue *FName = MF.getFunction();
    BuildMI(MBB, I, DL, TII.get(M6502::LUi64), V0)
      .addGlobalAddress(FName, 0, M6502II::MO_GPOFF_HI);
    BuildMI(MBB, I, DL, TII.get(M6502::DADDu), V1).addReg(V0)
      .addReg(M6502::T9_64);
    BuildMI(MBB, I, DL, TII.get(M6502::DADDiu), GlobalBaseReg).addReg(V1)
      .addGlobalAddress(FName, 0, M6502II::MO_GPOFF_LO);
    return;
  }

  if (!MF.getTarget().isPositionIndependent()) {
    // Set global register to __gnu_local_gp.
    //
    // lui   $v0, %hi(__gnu_local_gp)
    // addiu $globalbasereg, $v0, %lo(__gnu_local_gp)
    BuildMI(MBB, I, DL, TII.get(M6502::LUi), V0)
      .addExternalSymbol("__gnu_local_gp", M6502II::MO_ABS_HI);
    BuildMI(MBB, I, DL, TII.get(M6502::ADDiu), GlobalBaseReg).addReg(V0)
      .addExternalSymbol("__gnu_local_gp", M6502II::MO_ABS_LO);
    return;
  }

  MF.getRegInfo().addLiveIn(M6502::T9);
  MBB.addLiveIn(M6502::T9);

  if (ABI.IsN32()) {
    // lui $v0, %hi(%neg(%gp_rel(fname)))
    // addu $v1, $v0, $t9
    // addiu $globalbasereg, $v1, %lo(%neg(%gp_rel(fname)))
    const GlobalValue *FName = MF.getFunction();
    BuildMI(MBB, I, DL, TII.get(M6502::LUi), V0)
      .addGlobalAddress(FName, 0, M6502II::MO_GPOFF_HI);
    BuildMI(MBB, I, DL, TII.get(M6502::ADDu), V1).addReg(V0).addReg(M6502::T9);
    BuildMI(MBB, I, DL, TII.get(M6502::ADDiu), GlobalBaseReg).addReg(V1)
      .addGlobalAddress(FName, 0, M6502II::MO_GPOFF_LO);
    return;
  }

  assert(ABI.IsO32());

  // For O32 ABI, the following instruction sequence is emitted to initialize
  // the global base register:
  //
  //  0. lui   $2, %hi(_gp_disp)
  //  1. addiu $2, $2, %lo(_gp_disp)
  //  2. addu  $globalbasereg, $2, $t9
  //
  // We emit only the last instruction here.
  //
  // GNU linker requires that the first two instructions appear at the beginning
  // of a function and no instructions be inserted before or between them.
  // The two instructions are emitted during lowering to MC layer in order to
  // avoid any reordering.
  //
  // Register $2 (M6502::V0) is added to the list of live-in registers to ensure
  // the value instruction 1 (addiu) defines is valid when instruction 2 (addu)
  // reads it.
  MF.getRegInfo().addLiveIn(M6502::V0);
  MBB.addLiveIn(M6502::V0);
  BuildMI(MBB, I, DL, TII.get(M6502::ADDu), GlobalBaseReg)
    .addReg(M6502::V0).addReg(M6502::T9);
}

void M6502SEDAGToDAGISel::processFunctionAfterISel(MachineFunction &MF) {
  initGlobalBaseReg(MF);

  MachineRegisterInfo *MRI = &MF.getRegInfo();

  for (auto &MBB: MF) {
    for (auto &MI: MBB) {
      switch (MI.getOpcode()) {
      case M6502::RDDSP:
        addDSPCtrlRegOperands(false, MI, MF);
        break;
      case M6502::WRDSP:
        addDSPCtrlRegOperands(true, MI, MF);
        break;
      default:
        replaceUsesWithZeroReg(MRI, MI);
      }
    }
  }
}

void M6502SEDAGToDAGISel::selectAddE(SDNode *Node, const SDLoc &DL) const {
  SDValue InFlag = Node->getOperand(2);
  unsigned Opc = InFlag.getOpcode();
  SDValue LHS = Node->getOperand(0), RHS = Node->getOperand(1);
  EVT VT = LHS.getValueType();

  // In the base case, we can rely on the carry bit from the addsc
  // instruction.
  if (Opc == ISD::ADDC) {
    SDValue Ops[3] = {LHS, RHS, InFlag};
    CurDAG->SelectNodeTo(Node, M6502::ADDWC, VT, MVT::Glue, Ops);
    return;
  }

  assert(Opc == ISD::ADDE && "ISD::ADDE not in a chain of ADDE nodes!");

  // The more complex case is when there is a chain of ISD::ADDE nodes like:
  // (adde (adde (adde (addc a b) c) d) e).
  //
  // The addwc instruction does not write to the carry bit, instead it writes
  // to bit 20 of the dsp control register. To match this series of nodes, each
  // intermediate adde node must be expanded to write the carry bit before the
  // addition.

  // Start by reading the overflow field for addsc and moving the value to the
  // carry field. The usage of 1 here with M6502ISD::RDDSP / M6502::WRDSP
  // corresponds to reading/writing the entire control register to/from a GPR.

  SDValue CstOne = CurDAG->getTargetConstant(1, DL, MVT::i32);

  SDValue OuFlag = CurDAG->getTargetConstant(20, DL, MVT::i32);

  SDNode *DSPCtrlField =
      CurDAG->getMachineNode(M6502::RDDSP, DL, MVT::i32, MVT::Glue, CstOne, InFlag);

  SDNode *Carry = CurDAG->getMachineNode(
      M6502::EXT, DL, MVT::i32, SDValue(DSPCtrlField, 0), OuFlag, CstOne);

  SDValue Ops[4] = {SDValue(DSPCtrlField, 0),
                    CurDAG->getTargetConstant(6, DL, MVT::i32), CstOne,
                    SDValue(Carry, 0)};
  SDNode *DSPCFWithCarry = CurDAG->getMachineNode(M6502::INS, DL, MVT::i32, Ops);

  // My reading of the the M6502 DSP 3.01 specification isn't as clear as I
  // would like about whether bit 20 always gets overwritten by addwc.
  // Hence take an extremely conservative view and presume it's sticky. We
  // therefore need to clear it.

  SDValue Zero = CurDAG->getRegister(M6502::ZERO, MVT::i32);

  SDValue InsOps[4] = {Zero, OuFlag, CstOne, SDValue(DSPCFWithCarry, 0)};
  SDNode *DSPCtrlFinal = CurDAG->getMachineNode(M6502::INS, DL, MVT::i32, InsOps);

  SDNode *WrDSP = CurDAG->getMachineNode(M6502::WRDSP, DL, MVT::Glue,
                                         SDValue(DSPCtrlFinal, 0), CstOne);

  SDValue Operands[3] = {LHS, RHS, SDValue(WrDSP, 0)};
  CurDAG->SelectNodeTo(Node, M6502::ADDWC, VT, MVT::Glue, Operands);
}

/// Match frameindex
bool M6502SEDAGToDAGISel::selectAddrFrameIndex(SDValue Addr, SDValue &Base,
                                              SDValue &Offset) const {
  if (FrameIndexSDNode *FIN = dyn_cast<FrameIndexSDNode>(Addr)) {
    EVT ValTy = Addr.getValueType();

    Base   = CurDAG->getTargetFrameIndex(FIN->getIndex(), ValTy);
    Offset = CurDAG->getTargetConstant(0, SDLoc(Addr), ValTy);
    return true;
  }
  return false;
}

/// Match frameindex+offset and frameindex|offset
bool M6502SEDAGToDAGISel::selectAddrFrameIndexOffset(
    SDValue Addr, SDValue &Base, SDValue &Offset, unsigned OffsetBits,
    unsigned ShiftAmount = 0) const {
  if (CurDAG->isBaseWithConstantOffset(Addr)) {
    ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Addr.getOperand(1));
    if (isIntN(OffsetBits + ShiftAmount, CN->getSExtValue())) {
      EVT ValTy = Addr.getValueType();

      // If the first operand is a FI, get the TargetFI Node
      if (FrameIndexSDNode *FIN =
              dyn_cast<FrameIndexSDNode>(Addr.getOperand(0)))
        Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), ValTy);
      else {
        Base = Addr.getOperand(0);
        // If base is a FI, additional offset calculation is done in
        // eliminateFrameIndex, otherwise we need to check the alignment
        if (OffsetToAlignment(CN->getZExtValue(), 1ull << ShiftAmount) != 0)
          return false;
      }

      Offset = CurDAG->getTargetConstant(CN->getZExtValue(), SDLoc(Addr),
                                         ValTy);
      return true;
    }
  }
  return false;
}

/// ComplexPattern used on M6502InstrInfo
/// Used on M6502 Load/Store instructions
bool M6502SEDAGToDAGISel::selectAddrRegImm(SDValue Addr, SDValue &Base,
                                          SDValue &Offset) const {
  // if Address is FI, get the TargetFrameIndex.
  if (selectAddrFrameIndex(Addr, Base, Offset))
    return true;

  // on PIC code Load GA
  if (Addr.getOpcode() == M6502ISD::Wrapper) {
    Base   = Addr.getOperand(0);
    Offset = Addr.getOperand(1);
    return true;
  }

  if (!TM.isPositionIndependent()) {
    if ((Addr.getOpcode() == ISD::TargetExternalSymbol ||
        Addr.getOpcode() == ISD::TargetGlobalAddress))
      return false;
  }

  // Addresses of the form FI+const or FI|const
  if (selectAddrFrameIndexOffset(Addr, Base, Offset, 16))
    return true;

  // Operand is a result from an ADD.
  if (Addr.getOpcode() == ISD::ADD) {
    // When loading from constant pools, load the lower address part in
    // the instruction itself. Example, instead of:
    //  lui $2, %hi($CPI1_0)
    //  addiu $2, $2, %lo($CPI1_0)
    //  lwc1 $f0, 0($2)
    // Generate:
    //  lui $2, %hi($CPI1_0)
    //  lwc1 $f0, %lo($CPI1_0)($2)
    if (Addr.getOperand(1).getOpcode() == M6502ISD::Lo ||
        Addr.getOperand(1).getOpcode() == M6502ISD::GPRel) {
      SDValue Opnd0 = Addr.getOperand(1).getOperand(0);
      if (isa<ConstantPoolSDNode>(Opnd0) || isa<GlobalAddressSDNode>(Opnd0) ||
          isa<JumpTableSDNode>(Opnd0)) {
        Base = Addr.getOperand(0);
        Offset = Opnd0;
        return true;
      }
    }
  }

  return false;
}

/// ComplexPattern used on M6502InstrInfo
/// Used on M6502 Load/Store instructions
bool M6502SEDAGToDAGISel::selectAddrDefault(SDValue Addr, SDValue &Base,
                                           SDValue &Offset) const {
  Base = Addr;
  Offset = CurDAG->getTargetConstant(0, SDLoc(Addr), Addr.getValueType());
  return true;
}

bool M6502SEDAGToDAGISel::selectIntAddr(SDValue Addr, SDValue &Base,
                                       SDValue &Offset) const {
  return selectAddrRegImm(Addr, Base, Offset) ||
    selectAddrDefault(Addr, Base, Offset);
}

bool M6502SEDAGToDAGISel::selectAddrRegImm9(SDValue Addr, SDValue &Base,
                                           SDValue &Offset) const {
  if (selectAddrFrameIndex(Addr, Base, Offset))
    return true;

  if (selectAddrFrameIndexOffset(Addr, Base, Offset, 9))
    return true;

  return false;
}

/// Used on microM6502 LWC2, LDC2, SWC2 and SDC2 instructions (11-bit offset)
bool M6502SEDAGToDAGISel::selectAddrRegImm11(SDValue Addr, SDValue &Base,
                                            SDValue &Offset) const {
  if (selectAddrFrameIndex(Addr, Base, Offset))
    return true;

  if (selectAddrFrameIndexOffset(Addr, Base, Offset, 11))
    return true;

  return false;
}

/// Used on microM6502 Load/Store unaligned instructions (12-bit offset)
bool M6502SEDAGToDAGISel::selectAddrRegImm12(SDValue Addr, SDValue &Base,
                                            SDValue &Offset) const {
  if (selectAddrFrameIndex(Addr, Base, Offset))
    return true;

  if (selectAddrFrameIndexOffset(Addr, Base, Offset, 12))
    return true;

  return false;
}

bool M6502SEDAGToDAGISel::selectAddrRegImm16(SDValue Addr, SDValue &Base,
                                            SDValue &Offset) const {
  if (selectAddrFrameIndex(Addr, Base, Offset))
    return true;

  if (selectAddrFrameIndexOffset(Addr, Base, Offset, 16))
    return true;

  return false;
}

bool M6502SEDAGToDAGISel::selectIntAddr11MM(SDValue Addr, SDValue &Base,
                                         SDValue &Offset) const {
  return selectAddrRegImm11(Addr, Base, Offset) ||
    selectAddrDefault(Addr, Base, Offset);
}

bool M6502SEDAGToDAGISel::selectIntAddr12MM(SDValue Addr, SDValue &Base,
                                         SDValue &Offset) const {
  return selectAddrRegImm12(Addr, Base, Offset) ||
    selectAddrDefault(Addr, Base, Offset);
}

bool M6502SEDAGToDAGISel::selectIntAddr16MM(SDValue Addr, SDValue &Base,
                                         SDValue &Offset) const {
  return selectAddrRegImm16(Addr, Base, Offset) ||
    selectAddrDefault(Addr, Base, Offset);
}

bool M6502SEDAGToDAGISel::selectIntAddrLSL2MM(SDValue Addr, SDValue &Base,
                                             SDValue &Offset) const {
  if (selectAddrFrameIndexOffset(Addr, Base, Offset, 7)) {
    if (isa<FrameIndexSDNode>(Base))
      return false;

    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Offset)) {
      unsigned CnstOff = CN->getZExtValue();
      return (CnstOff == (CnstOff & 0x3c));
    }

    return false;
  }

  // For all other cases where "lw" would be selected, don't select "lw16"
  // because it would result in additional instructions to prepare operands.
  if (selectAddrRegImm(Addr, Base, Offset))
    return false;

  return selectAddrDefault(Addr, Base, Offset);
}

bool M6502SEDAGToDAGISel::selectIntAddrSImm10(SDValue Addr, SDValue &Base,
                                             SDValue &Offset) const {

  if (selectAddrFrameIndex(Addr, Base, Offset))
    return true;

  if (selectAddrFrameIndexOffset(Addr, Base, Offset, 10))
    return true;

  return selectAddrDefault(Addr, Base, Offset);
}

bool M6502SEDAGToDAGISel::selectIntAddrSImm10Lsl1(SDValue Addr, SDValue &Base,
                                                 SDValue &Offset) const {
  if (selectAddrFrameIndex(Addr, Base, Offset))
    return true;

  if (selectAddrFrameIndexOffset(Addr, Base, Offset, 10, 1))
    return true;

  return selectAddrDefault(Addr, Base, Offset);
}

bool M6502SEDAGToDAGISel::selectIntAddrSImm10Lsl2(SDValue Addr, SDValue &Base,
                                                 SDValue &Offset) const {
  if (selectAddrFrameIndex(Addr, Base, Offset))
    return true;

  if (selectAddrFrameIndexOffset(Addr, Base, Offset, 10, 2))
    return true;

  return selectAddrDefault(Addr, Base, Offset);
}

bool M6502SEDAGToDAGISel::selectIntAddrSImm10Lsl3(SDValue Addr, SDValue &Base,
                                                 SDValue &Offset) const {
  if (selectAddrFrameIndex(Addr, Base, Offset))
    return true;

  if (selectAddrFrameIndexOffset(Addr, Base, Offset, 10, 3))
    return true;

  return selectAddrDefault(Addr, Base, Offset);
}

// Select constant vector splats.
//
// Returns true and sets Imm if:
// * MSA is enabled
// * N is a ISD::BUILD_VECTOR representing a constant splat
bool M6502SEDAGToDAGISel::selectVSplat(SDNode *N, APInt &Imm,
                                      unsigned MinSizeInBits) const {
  if (!Subtarget->hasMSA())
    return false;

  BuildVectorSDNode *Node = dyn_cast<BuildVectorSDNode>(N);

  if (!Node)
    return false;

  APInt SplatValue, SplatUndef;
  unsigned SplatBitSize;
  bool HasAnyUndefs;

  if (!Node->isConstantSplat(SplatValue, SplatUndef, SplatBitSize, HasAnyUndefs,
                             MinSizeInBits, !Subtarget->isLittle()))
    return false;

  Imm = SplatValue;

  return true;
}

// Select constant vector splats.
//
// In addition to the requirements of selectVSplat(), this function returns
// true and sets Imm if:
// * The splat value is the same width as the elements of the vector
// * The splat value fits in an integer with the specified signed-ness and
//   width.
//
// This function looks through ISD::BITCAST nodes.
// TODO: This might not be appropriate for big-endian MSA since BITCAST is
//       sometimes a shuffle in big-endian mode.
//
// It's worth noting that this function is not used as part of the selection
// of ldi.[bhwd] since it does not permit using the wrong-typed ldi.[bhwd]
// instruction to achieve the desired bit pattern. ldi.[bhwd] is selected in
// M6502SEDAGToDAGISel::selectNode.
bool M6502SEDAGToDAGISel::
selectVSplatCommon(SDValue N, SDValue &Imm, bool Signed,
                   unsigned ImmBitSize) const {
  APInt ImmValue;
  EVT EltTy = N->getValueType(0).getVectorElementType();

  if (N->getOpcode() == ISD::BITCAST)
    N = N->getOperand(0);

  if (selectVSplat(N.getNode(), ImmValue, EltTy.getSizeInBits()) &&
      ImmValue.getBitWidth() == EltTy.getSizeInBits()) {

    if (( Signed && ImmValue.isSignedIntN(ImmBitSize)) ||
        (!Signed && ImmValue.isIntN(ImmBitSize))) {
      Imm = CurDAG->getTargetConstant(ImmValue, SDLoc(N), EltTy);
      return true;
    }
  }

  return false;
}

// Select constant vector splats.
bool M6502SEDAGToDAGISel::
selectVSplatUimm1(SDValue N, SDValue &Imm) const {
  return selectVSplatCommon(N, Imm, false, 1);
}

bool M6502SEDAGToDAGISel::
selectVSplatUimm2(SDValue N, SDValue &Imm) const {
  return selectVSplatCommon(N, Imm, false, 2);
}

bool M6502SEDAGToDAGISel::
selectVSplatUimm3(SDValue N, SDValue &Imm) const {
  return selectVSplatCommon(N, Imm, false, 3);
}

// Select constant vector splats.
bool M6502SEDAGToDAGISel::
selectVSplatUimm4(SDValue N, SDValue &Imm) const {
  return selectVSplatCommon(N, Imm, false, 4);
}

// Select constant vector splats.
bool M6502SEDAGToDAGISel::
selectVSplatUimm5(SDValue N, SDValue &Imm) const {
  return selectVSplatCommon(N, Imm, false, 5);
}

// Select constant vector splats.
bool M6502SEDAGToDAGISel::
selectVSplatUimm6(SDValue N, SDValue &Imm) const {
  return selectVSplatCommon(N, Imm, false, 6);
}

// Select constant vector splats.
bool M6502SEDAGToDAGISel::
selectVSplatUimm8(SDValue N, SDValue &Imm) const {
  return selectVSplatCommon(N, Imm, false, 8);
}

// Select constant vector splats.
bool M6502SEDAGToDAGISel::
selectVSplatSimm5(SDValue N, SDValue &Imm) const {
  return selectVSplatCommon(N, Imm, true, 5);
}

// Select constant vector splats whose value is a power of 2.
//
// In addition to the requirements of selectVSplat(), this function returns
// true and sets Imm if:
// * The splat value is the same width as the elements of the vector
// * The splat value is a power of two.
//
// This function looks through ISD::BITCAST nodes.
// TODO: This might not be appropriate for big-endian MSA since BITCAST is
//       sometimes a shuffle in big-endian mode.
bool M6502SEDAGToDAGISel::selectVSplatUimmPow2(SDValue N, SDValue &Imm) const {
  APInt ImmValue;
  EVT EltTy = N->getValueType(0).getVectorElementType();

  if (N->getOpcode() == ISD::BITCAST)
    N = N->getOperand(0);

  if (selectVSplat(N.getNode(), ImmValue, EltTy.getSizeInBits()) &&
      ImmValue.getBitWidth() == EltTy.getSizeInBits()) {
    int32_t Log2 = ImmValue.exactLogBase2();

    if (Log2 != -1) {
      Imm = CurDAG->getTargetConstant(Log2, SDLoc(N), EltTy);
      return true;
    }
  }

  return false;
}

// Select constant vector splats whose value only has a consecutive sequence
// of left-most bits set (e.g. 0b11...1100...00).
//
// In addition to the requirements of selectVSplat(), this function returns
// true and sets Imm if:
// * The splat value is the same width as the elements of the vector
// * The splat value is a consecutive sequence of left-most bits.
//
// This function looks through ISD::BITCAST nodes.
// TODO: This might not be appropriate for big-endian MSA since BITCAST is
//       sometimes a shuffle in big-endian mode.
bool M6502SEDAGToDAGISel::selectVSplatMaskL(SDValue N, SDValue &Imm) const {
  APInt ImmValue;
  EVT EltTy = N->getValueType(0).getVectorElementType();

  if (N->getOpcode() == ISD::BITCAST)
    N = N->getOperand(0);

  if (selectVSplat(N.getNode(), ImmValue, EltTy.getSizeInBits()) &&
      ImmValue.getBitWidth() == EltTy.getSizeInBits()) {
    // Extract the run of set bits starting with bit zero from the bitwise
    // inverse of ImmValue, and test that the inverse of this is the same
    // as the original value.
    if (ImmValue == ~(~ImmValue & ~(~ImmValue + 1))) {

      Imm = CurDAG->getTargetConstant(ImmValue.countPopulation() - 1, SDLoc(N),
                                      EltTy);
      return true;
    }
  }

  return false;
}

// Select constant vector splats whose value only has a consecutive sequence
// of right-most bits set (e.g. 0b00...0011...11).
//
// In addition to the requirements of selectVSplat(), this function returns
// true and sets Imm if:
// * The splat value is the same width as the elements of the vector
// * The splat value is a consecutive sequence of right-most bits.
//
// This function looks through ISD::BITCAST nodes.
// TODO: This might not be appropriate for big-endian MSA since BITCAST is
//       sometimes a shuffle in big-endian mode.
bool M6502SEDAGToDAGISel::selectVSplatMaskR(SDValue N, SDValue &Imm) const {
  APInt ImmValue;
  EVT EltTy = N->getValueType(0).getVectorElementType();

  if (N->getOpcode() == ISD::BITCAST)
    N = N->getOperand(0);

  if (selectVSplat(N.getNode(), ImmValue, EltTy.getSizeInBits()) &&
      ImmValue.getBitWidth() == EltTy.getSizeInBits()) {
    // Extract the run of set bits starting with bit zero, and test that the
    // result is the same as the original value
    if (ImmValue == (ImmValue & ~(ImmValue + 1))) {
      Imm = CurDAG->getTargetConstant(ImmValue.countPopulation() - 1, SDLoc(N),
                                      EltTy);
      return true;
    }
  }

  return false;
}

bool M6502SEDAGToDAGISel::selectVSplatUimmInvPow2(SDValue N,
                                                 SDValue &Imm) const {
  APInt ImmValue;
  EVT EltTy = N->getValueType(0).getVectorElementType();

  if (N->getOpcode() == ISD::BITCAST)
    N = N->getOperand(0);

  if (selectVSplat(N.getNode(), ImmValue, EltTy.getSizeInBits()) &&
      ImmValue.getBitWidth() == EltTy.getSizeInBits()) {
    int32_t Log2 = (~ImmValue).exactLogBase2();

    if (Log2 != -1) {
      Imm = CurDAG->getTargetConstant(Log2, SDLoc(N), EltTy);
      return true;
    }
  }

  return false;
}

bool M6502SEDAGToDAGISel::trySelect(SDNode *Node) {
  unsigned Opcode = Node->getOpcode();
  SDLoc DL(Node);

  ///
  // Instruction Selection not handled by the auto-generated
  // tablegen selection should be handled here.
  ///
  switch(Opcode) {
  default: break;

  case ISD::ADDE: {
    selectAddE(Node, DL);
    return true;
  }

  case ISD::ConstantFP: {
    ConstantFPSDNode *CN = dyn_cast<ConstantFPSDNode>(Node);
    if (Node->getValueType(0) == MVT::f64 && CN->isExactlyValue(+0.0)) {
      if (Subtarget->isGP64bit()) {
        SDValue Zero = CurDAG->getCopyFromReg(CurDAG->getEntryNode(), DL,
                                              M6502::ZERO_64, MVT::i64);
        ReplaceNode(Node,
                    CurDAG->getMachineNode(M6502::DMTC1, DL, MVT::f64, Zero));
      } else if (Subtarget->isFP64bit()) {
        SDValue Zero = CurDAG->getCopyFromReg(CurDAG->getEntryNode(), DL,
                                              M6502::ZERO, MVT::i32);
        ReplaceNode(Node, CurDAG->getMachineNode(M6502::BuildPairF64_64, DL,
                                                 MVT::f64, Zero, Zero));
      } else {
        SDValue Zero = CurDAG->getCopyFromReg(CurDAG->getEntryNode(), DL,
                                              M6502::ZERO, MVT::i32);
        ReplaceNode(Node, CurDAG->getMachineNode(M6502::BuildPairF64, DL,
                                                 MVT::f64, Zero, Zero));
      }
      return true;
    }
    break;
  }

  case ISD::Constant: {
    const ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Node);
    int64_t Imm = CN->getSExtValue();
    unsigned Size = CN->getValueSizeInBits(0);

    if (isInt<32>(Imm))
      break;

    M6502AnalyzeImmediate AnalyzeImm;

    const M6502AnalyzeImmediate::InstSeq &Seq =
      AnalyzeImm.Analyze(Imm, Size, false);

    M6502AnalyzeImmediate::InstSeq::const_iterator Inst = Seq.begin();
    SDLoc DL(CN);
    SDNode *RegOpnd;
    SDValue ImmOpnd = CurDAG->getTargetConstant(SignExtend64<16>(Inst->ImmOpnd),
                                                DL, MVT::i64);

    // The first instruction can be a LUi which is different from other
    // instructions (ADDiu, ORI and SLL) in that it does not have a register
    // operand.
    if (Inst->Opc == M6502::LUi64)
      RegOpnd = CurDAG->getMachineNode(Inst->Opc, DL, MVT::i64, ImmOpnd);
    else
      RegOpnd =
        CurDAG->getMachineNode(Inst->Opc, DL, MVT::i64,
                               CurDAG->getRegister(M6502::ZERO_64, MVT::i64),
                               ImmOpnd);

    // The remaining instructions in the sequence are handled here.
    for (++Inst; Inst != Seq.end(); ++Inst) {
      ImmOpnd = CurDAG->getTargetConstant(SignExtend64<16>(Inst->ImmOpnd), DL,
                                          MVT::i64);
      RegOpnd = CurDAG->getMachineNode(Inst->Opc, DL, MVT::i64,
                                       SDValue(RegOpnd, 0), ImmOpnd);
    }

    ReplaceNode(Node, RegOpnd);
    return true;
  }

  case ISD::INTRINSIC_W_CHAIN: {
    switch (cast<ConstantSDNode>(Node->getOperand(1))->getZExtValue()) {
    default:
      break;

    case Intrinsic::m6502_cfcmsa: {
      SDValue ChainIn = Node->getOperand(0);
      SDValue RegIdx = Node->getOperand(2);
      SDValue Reg = CurDAG->getCopyFromReg(ChainIn, DL,
                                           getMSACtrlReg(RegIdx), MVT::i32);
      ReplaceNode(Node, Reg.getNode());
      return true;
    }
    }
    break;
  }

  case ISD::INTRINSIC_WO_CHAIN: {
    switch (cast<ConstantSDNode>(Node->getOperand(0))->getZExtValue()) {
    default:
      break;

    case Intrinsic::m6502_move_v:
      // Like an assignment but will always produce a move.v even if
      // unnecessary.
      ReplaceNode(Node, CurDAG->getMachineNode(M6502::MOVE_V, DL,
                                               Node->getValueType(0),
                                               Node->getOperand(1)));
      return true;
    }
    break;
  }

  case ISD::INTRINSIC_VOID: {
    switch (cast<ConstantSDNode>(Node->getOperand(1))->getZExtValue()) {
    default:
      break;

    case Intrinsic::m6502_ctcmsa: {
      SDValue ChainIn = Node->getOperand(0);
      SDValue RegIdx  = Node->getOperand(2);
      SDValue Value   = Node->getOperand(3);
      SDValue ChainOut = CurDAG->getCopyToReg(ChainIn, DL,
                                              getMSACtrlReg(RegIdx), Value);
      ReplaceNode(Node, ChainOut.getNode());
      return true;
    }
    }
    break;
  }

  // Manually match M6502ISD::Ins nodes to get the correct instruction. It has
  // to be done in this fashion so that we respect the differences between
  // dins and dinsm, as the difference is that the size operand has the range
  // 0 < size <= 32 for dins while dinsm has the range 2 <= size <= 64 which
  // means SelectionDAGISel would have to test all the operands at once to
  // match the instruction.
  case M6502ISD::Ins: {

    // Sanity checking for the node operands.
    if (Node->getValueType(0) != MVT::i32 && Node->getValueType(0) != MVT::i64)
      return false;

    if (Node->getNumOperands() != 4)
      return false;

    if (Node->getOperand(1)->getOpcode() != ISD::Constant ||
        Node->getOperand(2)->getOpcode() != ISD::Constant)
      return false;

    MVT ResTy = Node->getSimpleValueType(0);
    uint64_t Pos = Node->getConstantOperandVal(1);
    uint64_t Size = Node->getConstantOperandVal(2);

    // Size has to be >0 for 'ins', 'dins' and 'dinsu'.
    if (!Size)
      return false;

    if (Pos + Size > 64)
      return false;

    if (ResTy != MVT::i32 && ResTy != MVT::i64)
      return false;

    unsigned Opcode = 0;
    if (ResTy == MVT::i32) {
      if (Pos + Size <= 32)
        Opcode = M6502::INS;
    } else {
      if (Pos + Size <= 32)
        Opcode = M6502::DINS;
      else if (Pos < 32 && 1 < Size)
        Opcode = M6502::DINSM;
      else
        Opcode = M6502::DINSU;
    }

    if (Opcode) {
      SDValue Ops[4] = {
          Node->getOperand(0), CurDAG->getTargetConstant(Pos, DL, MVT::i32),
          CurDAG->getTargetConstant(Size, DL, MVT::i32), Node->getOperand(3)};

      ReplaceNode(Node, CurDAG->getMachineNode(Opcode, DL, ResTy, Ops));
      return true;
    }

    return false;
  }

  case M6502ISD::ThreadPointer: {
    EVT PtrVT = getTargetLowering()->getPointerTy(CurDAG->getDataLayout());
    unsigned RdhwrOpc, DestReg;

    if (PtrVT == MVT::i32) {
      RdhwrOpc = M6502::RDHWR;
      DestReg = M6502::V1;
    } else {
      RdhwrOpc = M6502::RDHWR64;
      DestReg = M6502::V1_64;
    }

    SDNode *Rdhwr =
      CurDAG->getMachineNode(RdhwrOpc, DL,
                             Node->getValueType(0),
                             CurDAG->getRegister(M6502::HWR29, MVT::i32));
    SDValue Chain = CurDAG->getCopyToReg(CurDAG->getEntryNode(), DL, DestReg,
                                         SDValue(Rdhwr, 0));
    SDValue ResNode = CurDAG->getCopyFromReg(Chain, DL, DestReg, PtrVT);
    ReplaceNode(Node, ResNode.getNode());
    return true;
  }

  case ISD::BUILD_VECTOR: {
    // Select appropriate ldi.[bhwd] instructions for constant splats of
    // 128-bit when MSA is enabled. Fixup any register class mismatches that
    // occur as a result.
    //
    // This allows the compiler to use a wider range of immediates than would
    // otherwise be allowed. If, for example, v4i32 could only use ldi.h then
    // it would not be possible to load { 0x01010101, 0x01010101, 0x01010101,
    // 0x01010101 } without using a constant pool. This would be sub-optimal
    // when // 'ldi.b wd, 1' is capable of producing that bit-pattern in the
    // same set/ of registers. Similarly, ldi.h isn't capable of producing {
    // 0x00000000, 0x00000001, 0x00000000, 0x00000001 } but 'ldi.d wd, 1' can.

    const M6502ABIInfo &ABI =
        static_cast<const M6502TargetMachine &>(TM).getABI();

    BuildVectorSDNode *BVN = cast<BuildVectorSDNode>(Node);
    APInt SplatValue, SplatUndef;
    unsigned SplatBitSize;
    bool HasAnyUndefs;
    unsigned LdiOp;
    EVT ResVecTy = BVN->getValueType(0);
    EVT ViaVecTy;

    if (!Subtarget->hasMSA() || !BVN->getValueType(0).is128BitVector())
      return false;

    if (!BVN->isConstantSplat(SplatValue, SplatUndef, SplatBitSize,
                              HasAnyUndefs, 8,
                              !Subtarget->isLittle()))
      return false;

    switch (SplatBitSize) {
    default:
      return false;
    case 8:
      LdiOp = M6502::LDI_B;
      ViaVecTy = MVT::v16i8;
      break;
    case 16:
      LdiOp = M6502::LDI_H;
      ViaVecTy = MVT::v8i16;
      break;
    case 32:
      LdiOp = M6502::LDI_W;
      ViaVecTy = MVT::v4i32;
      break;
    case 64:
      LdiOp = M6502::LDI_D;
      ViaVecTy = MVT::v2i64;
      break;
    }

    SDNode *Res;

    // If we have a signed 10 bit integer, we can splat it directly.
    //
    // If we have something bigger we can synthesize the value into a GPR and
    // splat from there.
    if (SplatValue.isSignedIntN(10)) {
      SDValue Imm = CurDAG->getTargetConstant(SplatValue, DL,
                                              ViaVecTy.getVectorElementType());

      Res = CurDAG->getMachineNode(LdiOp, DL, ViaVecTy, Imm);
    } else if (SplatValue.isSignedIntN(16) &&
               ((ABI.IsO32() && SplatBitSize < 64) ||
                (ABI.IsN32() || ABI.IsN64()))) {
      // Only handle signed 16 bit values when the element size is GPR width.
      // M650264 can handle all the cases but M650232 would need to handle
      // negative cases specifically here. Instead, handle those cases as
      // 64bit values.

      bool Is32BitSplat = ABI.IsO32() || SplatBitSize < 64;
      const unsigned ADDiuOp = Is32BitSplat ? M6502::ADDiu : M6502::DADDiu;
      const MVT SplatMVT = Is32BitSplat ? MVT::i32 : MVT::i64;
      SDValue ZeroVal = CurDAG->getRegister(
          Is32BitSplat ? M6502::ZERO : M6502::ZERO_64, SplatMVT);

      const unsigned FILLOp =
          SplatBitSize == 16
              ? M6502::FILL_H
              : (SplatBitSize == 32 ? M6502::FILL_W
                                    : (SplatBitSize == 64 ? M6502::FILL_D : 0));

      assert(FILLOp != 0 && "Unknown FILL Op for splat synthesis!");
      assert((!ABI.IsO32() || (FILLOp != M6502::FILL_D)) &&
             "Attempting to use fill.d on M650232!");

      const unsigned Lo = SplatValue.getLoBits(16).getZExtValue();
      SDValue LoVal = CurDAG->getTargetConstant(Lo, DL, SplatMVT);

      Res = CurDAG->getMachineNode(ADDiuOp, DL, SplatMVT, ZeroVal, LoVal);
      Res = CurDAG->getMachineNode(FILLOp, DL, ViaVecTy, SDValue(Res, 0));

    } else if (SplatValue.isSignedIntN(32) && SplatBitSize == 32) {
      // Only handle the cases where the splat size agrees with the size
      // of the SplatValue here.
      const unsigned Lo = SplatValue.getLoBits(16).getZExtValue();
      const unsigned Hi = SplatValue.lshr(16).getLoBits(16).getZExtValue();
      SDValue ZeroVal = CurDAG->getRegister(M6502::ZERO, MVT::i32);

      SDValue LoVal = CurDAG->getTargetConstant(Lo, DL, MVT::i32);
      SDValue HiVal = CurDAG->getTargetConstant(Hi, DL, MVT::i32);

      if (Hi)
        Res = CurDAG->getMachineNode(M6502::LUi, DL, MVT::i32, HiVal);

      if (Lo)
        Res = CurDAG->getMachineNode(M6502::ORi, DL, MVT::i32,
                                     Hi ? SDValue(Res, 0) : ZeroVal, LoVal);

      assert((Hi || Lo) && "Zero case reached 32 bit case splat synthesis!");
      Res = CurDAG->getMachineNode(M6502::FILL_W, DL, MVT::v4i32, SDValue(Res, 0));

    } else if (SplatValue.isSignedIntN(32) && SplatBitSize == 64 &&
               (ABI.IsN32() || ABI.IsN64())) {
      // N32 and N64 can perform some tricks that O32 can't for signed 32 bit
      // integers due to having 64bit registers. lui will cause the necessary
      // zero/sign extension.
      const unsigned Lo = SplatValue.getLoBits(16).getZExtValue();
      const unsigned Hi = SplatValue.lshr(16).getLoBits(16).getZExtValue();
      SDValue ZeroVal = CurDAG->getRegister(M6502::ZERO, MVT::i32);

      SDValue LoVal = CurDAG->getTargetConstant(Lo, DL, MVT::i32);
      SDValue HiVal = CurDAG->getTargetConstant(Hi, DL, MVT::i32);

      if (Hi)
        Res = CurDAG->getMachineNode(M6502::LUi, DL, MVT::i32, HiVal);

      if (Lo)
        Res = CurDAG->getMachineNode(M6502::ORi, DL, MVT::i32,
                                     Hi ? SDValue(Res, 0) : ZeroVal, LoVal);

      Res = CurDAG->getMachineNode(
              M6502::SUBREG_TO_REG, DL, MVT::i64,
              CurDAG->getTargetConstant(((Hi >> 15) & 0x1), DL, MVT::i64),
              SDValue(Res, 0),
              CurDAG->getTargetConstant(M6502::sub_32, DL, MVT::i64));

      Res =
          CurDAG->getMachineNode(M6502::FILL_D, DL, MVT::v2i64, SDValue(Res, 0));

    } else if (SplatValue.isSignedIntN(64)) {
      // If we have a 64 bit Splat value, we perform a similar sequence to the
      // above:
      //
      // M650232:                            M650264:
      //   lui $res, %highest(val)            lui $res, %highest(val)
      //   ori $res, $res, %higher(val)       ori $res, $res, %higher(val)
      //   lui $res2, %hi(val)                lui $res2, %hi(val)
      //   ori $res2, %res2, %lo(val)         ori $res2, %res2, %lo(val)
      //   $res3 = fill $res2                 dinsu $res, $res2, 0, 32
      //   $res4 = insert.w $res3[1], $res    fill.d $res
      //   splat.d $res4, 0
      //
      // The ability to use dinsu is guaranteed as MSA requires M6502R5. This saves
      // having to materialize the value by shifts and ors.
      //
      // FIXME: Implement the preferred sequence for M650264R6:
      //
      // M650264R6:
      //   ori $res, $zero, %lo(val)
      //   daui $res, $res, %hi(val)
      //   dahi $res, $res, %higher(val)
      //   dati $res, $res, %highest(cal)
      //   fill.d $res
      //

      const unsigned Lo = SplatValue.getLoBits(16).getZExtValue();
      const unsigned Hi = SplatValue.lshr(16).getLoBits(16).getZExtValue();
      const unsigned Higher = SplatValue.lshr(32).getLoBits(16).getZExtValue();
      const unsigned Highest = SplatValue.lshr(48).getLoBits(16).getZExtValue();

      SDValue LoVal = CurDAG->getTargetConstant(Lo, DL, MVT::i32);
      SDValue HiVal = CurDAG->getTargetConstant(Hi, DL, MVT::i32);
      SDValue HigherVal = CurDAG->getTargetConstant(Higher, DL, MVT::i32);
      SDValue HighestVal = CurDAG->getTargetConstant(Highest, DL, MVT::i32);
      SDValue ZeroVal = CurDAG->getRegister(M6502::ZERO, MVT::i32);

      // Independent of whether we're targeting M650264 or not, the basic
      // operations are the same. Also, directly use the $zero register if
      // the 16 bit chunk is zero.
      //
      // For optimization purposes we always synthesize the splat value as
      // an i32 value, then if we're targetting M650264, use SUBREG_TO_REG
      // just before combining the values with dinsu to produce an i64. This
      // enables SelectionDAG to aggressively share components of splat values
      // where possible.
      //
      // FIXME: This is the general constant synthesis problem. This code
      //        should be factored out into a class shared between all the
      //        classes that need it. Specifically, for a splat size of 64
      //        bits that's a negative number we can do better than LUi/ORi
      //        for the upper 32bits.

      if (Hi)
        Res = CurDAG->getMachineNode(M6502::LUi, DL, MVT::i32, HiVal);

      if (Lo)
        Res = CurDAG->getMachineNode(M6502::ORi, DL, MVT::i32,
                                     Hi ? SDValue(Res, 0) : ZeroVal, LoVal);

      SDNode *HiRes;
      if (Highest)
        HiRes = CurDAG->getMachineNode(M6502::LUi, DL, MVT::i32, HighestVal);

      if (Higher)
        HiRes = CurDAG->getMachineNode(M6502::ORi, DL, MVT::i32,
                                       Highest ? SDValue(HiRes, 0) : ZeroVal,
                                       HigherVal);


      if (ABI.IsO32()) {
        Res = CurDAG->getMachineNode(M6502::FILL_W, DL, MVT::v4i32,
                                     (Hi || Lo) ? SDValue(Res, 0) : ZeroVal);

        Res = CurDAG->getMachineNode(
            M6502::INSERT_W, DL, MVT::v4i32, SDValue(Res, 0),
            (Highest || Higher) ? SDValue(HiRes, 0) : ZeroVal,
            CurDAG->getTargetConstant(1, DL, MVT::i32));

        const TargetLowering *TLI = getTargetLowering();
        const TargetRegisterClass *RC =
            TLI->getRegClassFor(ViaVecTy.getSimpleVT());

        Res = CurDAG->getMachineNode(
            M6502::COPY_TO_REGCLASS, DL, ViaVecTy, SDValue(Res, 0),
            CurDAG->getTargetConstant(RC->getID(), DL, MVT::i32));

        Res = CurDAG->getMachineNode(
            M6502::SPLATI_D, DL, MVT::v2i64, SDValue(Res, 0),
            CurDAG->getTargetConstant(0, DL, MVT::i32));
      } else if (ABI.IsN64() || ABI.IsN32()) {

        SDValue Zero64Val = CurDAG->getRegister(M6502::ZERO_64, MVT::i64);
        const bool HiResNonZero = Highest || Higher;
        const bool ResNonZero = Hi || Lo;

        if (HiResNonZero)
          HiRes = CurDAG->getMachineNode(
              M6502::SUBREG_TO_REG, DL, MVT::i64,
              CurDAG->getTargetConstant(((Highest >> 15) & 0x1), DL, MVT::i64),
              SDValue(HiRes, 0),
              CurDAG->getTargetConstant(M6502::sub_32, DL, MVT::i64));

        if (ResNonZero)
          Res = CurDAG->getMachineNode(
              M6502::SUBREG_TO_REG, DL, MVT::i64,
              CurDAG->getTargetConstant(((Hi >> 15) & 0x1), DL, MVT::i64),
              SDValue(Res, 0),
              CurDAG->getTargetConstant(M6502::sub_32, DL, MVT::i64));

        // We have 3 cases:
        //   The HiRes is nonzero but Res is $zero  => dsll32 HiRes, 0
        //   The Res is nonzero but HiRes is $zero  => dinsu Res, $zero, 32, 32
        //   Both are non zero                      => dinsu Res, HiRes, 32, 32
        //
        // The obvious "missing" case is when both are zero, but that case is
        // handled by the ldi case.
        if (ResNonZero) {
          IntegerType *Int32Ty =
              IntegerType::get(MF->getFunction()->getContext(), 32);
          const ConstantInt *Const32 = ConstantInt::get(Int32Ty, 32);
          SDValue Ops[4] = {HiResNonZero ? SDValue(HiRes, 0) : Zero64Val,
                            CurDAG->getConstant(*Const32, DL, MVT::i32),
                            CurDAG->getConstant(*Const32, DL, MVT::i32),
                            SDValue(Res, 0)};

          Res = CurDAG->getMachineNode(M6502::DINSU, DL, MVT::i64, Ops);
        } else if (HiResNonZero) {
          Res = CurDAG->getMachineNode(
              M6502::DSLL32, DL, MVT::i64, SDValue(HiRes, 0),
              CurDAG->getTargetConstant(0, DL, MVT::i32));
        } else
          llvm_unreachable(
              "Zero splat value handled by non-zero 64bit splat synthesis!");

        Res = CurDAG->getMachineNode(M6502::FILL_D, DL, MVT::v2i64, SDValue(Res, 0));
      } else
        llvm_unreachable("Unknown ABI in M6502ISelDAGToDAG!");

    } else
      return false;

    if (ResVecTy != ViaVecTy) {
      // If LdiOp is writing to a different register class to ResVecTy, then
      // fix it up here. This COPY_TO_REGCLASS should never cause a move.v
      // since the source and destination register sets contain the same
      // registers.
      const TargetLowering *TLI = getTargetLowering();
      MVT ResVecTySimple = ResVecTy.getSimpleVT();
      const TargetRegisterClass *RC = TLI->getRegClassFor(ResVecTySimple);
      Res = CurDAG->getMachineNode(M6502::COPY_TO_REGCLASS, DL,
                                   ResVecTy, SDValue(Res, 0),
                                   CurDAG->getTargetConstant(RC->getID(), DL,
                                                             MVT::i32));
    }

    ReplaceNode(Node, Res);
    return true;
  }

  }

  return false;
}

bool M6502SEDAGToDAGISel::
SelectInlineAsmMemoryOperand(const SDValue &Op, unsigned ConstraintID,
                             std::vector<SDValue> &OutOps) {
  SDValue Base, Offset;

  switch(ConstraintID) {
  default:
    llvm_unreachable("Unexpected asm memory constraint");
  // All memory constraints can at least accept raw pointers.
  case InlineAsm::Constraint_i:
    OutOps.push_back(Op);
    OutOps.push_back(CurDAG->getTargetConstant(0, SDLoc(Op), MVT::i32));
    return false;
  case InlineAsm::Constraint_m:
    if (selectAddrRegImm16(Op, Base, Offset)) {
      OutOps.push_back(Base);
      OutOps.push_back(Offset);
      return false;
    }
    OutOps.push_back(Op);
    OutOps.push_back(CurDAG->getTargetConstant(0, SDLoc(Op), MVT::i32));
    return false;
  case InlineAsm::Constraint_R:
    // The 'R' constraint is supposed to be much more complicated than this.
    // However, it's becoming less useful due to architectural changes and
    // ought to be replaced by other constraints such as 'ZC'.
    // For now, support 9-bit signed offsets which is supportable by all
    // subtargets for all instructions.
    if (selectAddrRegImm9(Op, Base, Offset)) {
      OutOps.push_back(Base);
      OutOps.push_back(Offset);
      return false;
    }
    OutOps.push_back(Op);
    OutOps.push_back(CurDAG->getTargetConstant(0, SDLoc(Op), MVT::i32));
    return false;
  case InlineAsm::Constraint_ZC:
    // ZC matches whatever the pref, ll, and sc instructions can handle for the
    // given subtarget.
    if (Subtarget->inMicroM6502Mode()) {
      // On microM6502, they can handle 12-bit offsets.
      if (selectAddrRegImm12(Op, Base, Offset)) {
        OutOps.push_back(Base);
        OutOps.push_back(Offset);
        return false;
      }
    } else if (Subtarget->hasM650232r6()) {
      // On M650232r6/M650264r6, they can only handle 9-bit offsets.
      if (selectAddrRegImm9(Op, Base, Offset)) {
        OutOps.push_back(Base);
        OutOps.push_back(Offset);
        return false;
      }
    } else if (selectAddrRegImm16(Op, Base, Offset)) {
      // Prior to M650232r6/M650264r6, they can handle 16-bit offsets.
      OutOps.push_back(Base);
      OutOps.push_back(Offset);
      return false;
    }
    // In all cases, 0-bit offsets are acceptable.
    OutOps.push_back(Op);
    OutOps.push_back(CurDAG->getTargetConstant(0, SDLoc(Op), MVT::i32));
    return false;
  }
  return true;
}

FunctionPass *llvm::createM6502SEISelDag(M6502TargetMachine &TM,
                                        CodeGenOpt::Level OptLevel) {
  return new M6502SEDAGToDAGISel(TM, OptLevel);
}
