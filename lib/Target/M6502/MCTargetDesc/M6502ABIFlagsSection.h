//===- M6502ABIFlagsSection.h - M6502 ELF ABI Flags Section -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_M6502_MCTARGETDESC_M6502ABIFLAGSSECTION_H
#define LLVM_LIB_TARGET_M6502_MCTARGETDESC_M6502ABIFLAGSSECTION_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MipsABIFlags.h"
#include <cstdint>

namespace llvm {

class MCStreamer;

struct M6502ABIFlagsSection {
  // Internal representation of the fp_abi related values used in .module.
  enum class FpABIKind { ANY, XX, S32, S64, SOFT };

  // Version of flags structure.
  uint16_t Version = 0;
  // The level of the ISA: 1-5, 32, 64.
  uint8_t ISALevel = 0;
  // The revision of ISA: 0 for M6502 V and below, 1-n otherwise.
  uint8_t ISARevision = 0;
  // The size of general purpose registers.
  Mips::AFL_REG GPRSize = Mips::AFL_REG_NONE;
  // The size of co-processor 1 registers.
  Mips::AFL_REG CPR1Size = Mips::AFL_REG_NONE;
  // The size of co-processor 2 registers.
  Mips::AFL_REG CPR2Size = Mips::AFL_REG_NONE;
  // Processor-specific extension.
  Mips::AFL_EXT ISAExtension = Mips::AFL_EXT_NONE;
  // Mask of ASEs used.
  uint32_t ASESet = 0;

  bool OddSPReg = false;

  bool Is32BitABI = false;

protected:
  // The floating-point ABI.
  FpABIKind FpABI = FpABIKind::ANY;

public:
  M6502ABIFlagsSection() = default;

  uint16_t getVersionValue() { return (uint16_t)Version; }
  uint8_t getISALevelValue() { return (uint8_t)ISALevel; }
  uint8_t getISARevisionValue() { return (uint8_t)ISARevision; }
  uint8_t getGPRSizeValue() { return (uint8_t)GPRSize; }
  uint8_t getCPR1SizeValue();
  uint8_t getCPR2SizeValue() { return (uint8_t)CPR2Size; }
  uint8_t getFpABIValue();
  uint32_t getISAExtensionValue() { return (uint32_t)ISAExtension; }
  uint32_t getASESetValue() { return (uint32_t)ASESet; }

  uint32_t getFlags1Value() {
    uint32_t Value = 0;

    if (OddSPReg)
      Value |= (uint32_t)Mips::AFL_FLAGS1_ODDSPREG;

    return Value;
  }

  uint32_t getFlags2Value() { return 0; }

  FpABIKind getFpABI() { return FpABI; }
  void setFpABI(FpABIKind Value, bool IsABI32Bit) {
    FpABI = Value;
    Is32BitABI = IsABI32Bit;
  }

  StringRef getFpABIString(FpABIKind Value);

  template <class PredicateLibrary>
  void setISALevelAndRevisionFromPredicates(const PredicateLibrary &P) {
    if (P.hasM650264()) {
      ISALevel = 64;
      if (P.hasM650264r6())
        ISARevision = 6;
      else if (P.hasM650264r5())
        ISARevision = 5;
      else if (P.hasM650264r3())
        ISARevision = 3;
      else if (P.hasM650264r2())
        ISARevision = 2;
      else
        ISARevision = 1;
    } else if (P.hasM650232()) {
      ISALevel = 32;
      if (P.hasM650232r6())
        ISARevision = 6;
      else if (P.hasM650232r5())
        ISARevision = 5;
      else if (P.hasM650232r3())
        ISARevision = 3;
      else if (P.hasM650232r2())
        ISARevision = 2;
      else
        ISARevision = 1;
    } else {
      ISARevision = 0;
      if (P.hasM65025())
        ISALevel = 5;
      else if (P.hasM65024())
        ISALevel = 4;
      else if (P.hasM65023())
        ISALevel = 3;
      else if (P.hasM65022())
        ISALevel = 2;
      else if (P.hasM65021())
        ISALevel = 1;
      else
        llvm_unreachable("Unknown ISA level!");
    }
  }

  template <class PredicateLibrary>
  void setGPRSizeFromPredicates(const PredicateLibrary &P) {
    GPRSize = P.isGP64bit() ? Mips::AFL_REG_64 : Mips::AFL_REG_32;
  }

  template <class PredicateLibrary>
  void setCPR1SizeFromPredicates(const PredicateLibrary &P) {
    if (P.useSoftFloat())
      CPR1Size = Mips::AFL_REG_NONE;
    else if (P.hasMSA())
      CPR1Size = Mips::AFL_REG_128;
    else
      CPR1Size = P.isFP64bit() ? Mips::AFL_REG_64 : Mips::AFL_REG_32;
  }

  template <class PredicateLibrary>
  void setISAExtensionFromPredicates(const PredicateLibrary &P) {
    if (P.hasCnM6502())
      ISAExtension = Mips::AFL_EXT_OCTEON;
    else
      ISAExtension = Mips::AFL_EXT_NONE;
  }

  template <class PredicateLibrary>
  void setASESetFromPredicates(const PredicateLibrary &P) {
    ASESet = 0;
    if (P.hasDSP())
      ASESet |= Mips::AFL_ASE_DSP;
    if (P.hasDSPR2())
      ASESet |= Mips::AFL_ASE_DSPR2;
    if (P.hasMSA())
      ASESet |= Mips::AFL_ASE_MSA;
    if (P.inMicroM6502Mode())
      ASESet |= Mips::AFL_ASE_MICROMIPS;
    if (P.inM650216Mode())
      ASESet |= Mips::AFL_ASE_MIPS16;
    if (P.hasMT())
      ASESet |= Mips::AFL_ASE_MT;
  }

  template <class PredicateLibrary>
  void setFpAbiFromPredicates(const PredicateLibrary &P) {
    Is32BitABI = P.isABI_O32();

    FpABI = FpABIKind::ANY;
    if (P.useSoftFloat())
      FpABI = FpABIKind::SOFT;
    else if (P.isABI_N32() || P.isABI_N64())
      FpABI = FpABIKind::S64;
    else if (P.isABI_O32()) {
      if (P.isABI_FPXX())
        FpABI = FpABIKind::XX;
      else if (P.isFP64bit())
        FpABI = FpABIKind::S64;
      else
        FpABI = FpABIKind::S32;
    }
  }

  template <class PredicateLibrary>
  void setAllFromPredicates(const PredicateLibrary &P) {
    setISALevelAndRevisionFromPredicates(P);
    setGPRSizeFromPredicates(P);
    setCPR1SizeFromPredicates(P);
    setISAExtensionFromPredicates(P);
    setASESetFromPredicates(P);
    setFpAbiFromPredicates(P);
    OddSPReg = P.useOddSPReg();
  }
};

MCStreamer &operator<<(MCStreamer &OS, M6502ABIFlagsSection &ABIFlagsSection);

} // end namespace llvm

#endif // LLVM_LIB_TARGET_M6502_MCTARGETDESC_M6502ABIFLAGSSECTION_H
