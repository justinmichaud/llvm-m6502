#include "SimpleMCTargetDesc.h"
#include "SimpleMCAsmInfo.h"
#include "llvm/MC/MCCodeGenInfo.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/TargetRegistry.h"

using namespace llvm;

#define GET_SUBTARGETINFO_MC_DESC
#include "SimpleGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "SimpleGenRegisterInfo.inc"

static MCAsmInfo *createSimpleMCAsmInfo(const MCRegisterInfo &MRI,
                                       StringRef TT) 
{
  MCAsmInfo *MAI = new SimpleMCAsmInfo(TT);
  return MAI;
}

// Force static initialization.
extern "C" void LLVMInitializeSimpleTargetMC() {
  // Register the MC asm info.
  RegisterMCAsmInfoFn X(TheSimpleTarget, createSimpleMCAsmInfo);
}
