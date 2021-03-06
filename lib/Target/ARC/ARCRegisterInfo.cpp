//===- ARCRegisterInfo.cpp - ARC Register Information -----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the ARC implementation of the MRegisterInfo class.
//
//===----------------------------------------------------------------------===//

#include "ARCRegisterInfo.h"
#include "ARC.h"
#include "ARCInstrInfo.h"
#include "ARCMachineFunctionInfo.h"
#include "ARCSubtarget.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/Debug.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

using namespace llvm;

#define DEBUG_TYPE "arc-reg-info"

#define GET_REGINFO_TARGET_DESC
#include "ARCGenRegisterInfo.inc"

static void ReplaceFrameIndex(MachineBasicBlock::iterator II,
                              const ARCInstrInfo &TII, unsigned Reg,
                              unsigned FrameReg, int Offset, int StackSize,
                              int ObjSize, RegScavenger *RS, int SPAdj) {
  assert(RS && "Need register scavenger.");
  MachineInstr &MI = *II;
  MachineBasicBlock &MBB = *MI.getParent();
  DebugLoc dl = MI.getDebugLoc();
  unsigned BaseReg = FrameReg;
  unsigned KillState = 0;
  if (MI.getOpcode() == ARC::LD_rs9 && (Offset >= 256 || Offset < -256)) {
    // Loads can always be reached with LD_rlimm.
    BuildMI(MBB, II, dl, TII.get(ARC::LD_rlimm), Reg)
        .addReg(BaseReg)
        .addImm(Offset)
        .addMemOperand(*MI.memoperands_begin());
    MBB.erase(II);
    return;
  }

  if (MI.getOpcode() != ARC::GETFI && (Offset >= 256 || Offset < -256)) {
    // We need to use a scratch register to reach the far-away frame indexes.
    BaseReg = RS->FindUnusedReg(&ARC::GPR32RegClass);
    if (!BaseReg) {
      // We can be sure that the scavenged-register slot is within the range
      // of the load offset.
      const TargetRegisterInfo *TRI =
          MBB.getParent()->getSubtarget().getRegisterInfo();
      BaseReg = RS->scavengeRegister(&ARC::GPR32RegClass, II, SPAdj);
      assert(BaseReg && "Register scavenging failed.");
      DEBUG(dbgs() << "Scavenged register " << PrintReg(BaseReg, TRI)
                   << " for FrameReg=" << PrintReg(FrameReg, TRI)
                   << "+Offset=" << Offset << "\n");
      (void)TRI;
      RS->setRegUsed(BaseReg);
    }
    unsigned AddOpc = isUInt<6>(Offset) ? ARC::ADD_rru6 : ARC::ADD_rrlimm;
    BuildMI(MBB, II, dl, TII.get(AddOpc))
        .addReg(BaseReg, RegState::Define)
        .addReg(FrameReg)
        .addImm(Offset);
    Offset = 0;
    KillState = RegState::Kill;
  }
  switch (MI.getOpcode()) {
  case ARC::LD_rs9:
    assert((Offset % 4 == 0) && "LD needs 4 byte alignment.");
  case ARC::LDH_rs9:
  case ARC::LDH_X_rs9:
    assert((Offset % 2 == 0) && "LDH needs 2 byte alignment.");
  case ARC::LDB_rs9:
  case ARC::LDB_X_rs9:
    DEBUG(dbgs() << "Building LDFI\n");
    BuildMI(MBB, II, dl, TII.get(MI.getOpcode()), Reg)
        .addReg(BaseReg, KillState)
        .addImm(Offset)
        .addMemOperand(*MI.memoperands_begin());
    break;
  case ARC::ST_rs9:
    assert((Offset % 4 == 0) && "ST needs 4 byte alignment.");
  case ARC::STH_rs9:
    assert((Offset % 2 == 0) && "STH needs 2 byte alignment.");
  case ARC::STB_rs9:
    DEBUG(dbgs() << "Building STFI\n");
    BuildMI(MBB, II, dl, TII.get(MI.getOpcode()))
        .addReg(Reg, getKillRegState(MI.getOperand(0).isKill()))
        .addReg(BaseReg, KillState)
        .addImm(Offset)
        .addMemOperand(*MI.memoperands_begin());
    break;
  case ARC::GETFI:
    DEBUG(dbgs() << "Building GETFI\n");
    BuildMI(MBB, II, dl,
            TII.get(isUInt<6>(Offset) ? ARC::ADD_rru6 : ARC::ADD_rrlimm))
        .addReg(Reg, RegState::Define)
        .addReg(FrameReg)
        .addImm(Offset);
    break;
  default:
    llvm_unreachable("Unhandled opcode.");
  }

  // Erase old instruction.
  MBB.erase(II);
}

ARCRegisterInfo::ARCRegisterInfo() : ARCGenRegisterInfo(ARC::BLINK) {}

bool ARCRegisterInfo::needsFrameMoves(const MachineFunction &MF) {
  return MF.getMMI().hasDebugInfo() ||
         MF.getFunction()->needsUnwindTableEntry();
}

const MCPhysReg *
ARCRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  return CSR_ARC_SaveList;
}

BitVector ARCRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());

  Reserved.set(ARC::ILINK);
  Reserved.set(ARC::SP);
  Reserved.set(ARC::GP);
  Reserved.set(ARC::R25);
  Reserved.set(ARC::BLINK);
  Reserved.set(ARC::FP);
  return Reserved;
}

bool ARCRegisterInfo::requiresRegisterScavenging(
    const MachineFunction &MF) const {
  return true;
}

bool ARCRegisterInfo::trackLivenessAfterRegAlloc(
    const MachineFunction &MF) const {
  return true;
}

bool ARCRegisterInfo::useFPForScavengingIndex(const MachineFunction &MF) const {
  return true;
}

void ARCRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                          int SPAdj, unsigned FIOperandNum,
                                          RegScavenger *RS) const {
  assert(SPAdj == 0 && "Unexpected");
  MachineInstr &MI = *II;
  MachineOperand &FrameOp = MI.getOperand(FIOperandNum);
  int FrameIndex = FrameOp.getIndex();

  MachineFunction &MF = *MI.getParent()->getParent();
  const ARCInstrInfo &TII = *MF.getSubtarget<ARCSubtarget>().getInstrInfo();
  const ARCFrameLowering *TFI = getFrameLowering(MF);
  int Offset = MF.getFrameInfo().getObjectOffset(FrameIndex);
  int ObjSize = MF.getFrameInfo().getObjectSize(FrameIndex);
  int StackSize = MF.getFrameInfo().getStackSize();
  int LocalFrameSize = MF.getFrameInfo().getLocalFrameSize();

  DEBUG(dbgs() << "\nFunction         : " << MF.getName() << "\n");
  DEBUG(dbgs() << "<--------->\n");
  DEBUG(dbgs() << MI << "\n");
  DEBUG(dbgs() << "FrameIndex         : " << FrameIndex << "\n");
  DEBUG(dbgs() << "ObjSize            : " << ObjSize << "\n");
  DEBUG(dbgs() << "FrameOffset        : " << Offset << "\n");
  DEBUG(dbgs() << "StackSize          : " << StackSize << "\n");
  DEBUG(dbgs() << "LocalFrameSize     : " << LocalFrameSize << "\n");
  (void)LocalFrameSize;

  // Special handling of DBG_VALUE instructions.
  if (MI.isDebugValue()) {
    unsigned FrameReg = getFrameRegister(MF);
    MI.getOperand(FIOperandNum).ChangeToRegister(FrameReg, false /*isDef*/);
    MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
    return;
  }

  // fold constant into offset.
  Offset += MI.getOperand(FIOperandNum + 1).getImm();

  // TODO: assert based on the load type:
  // ldb needs no alignment,
  // ldh needs 2 byte alignment
  // ld needs 4 byte alignment
  DEBUG(dbgs() << "Offset             : " << Offset << "\n"
               << "<--------->\n");

  unsigned Reg = MI.getOperand(0).getReg();
  assert(ARC::GPR32RegClass.contains(Reg) && "Unexpected register operand");

  if (!TFI->hasFP(MF)) {
    Offset = StackSize + Offset;
    if (FrameIndex >= 0)
      assert((Offset >= 0 && Offset < StackSize) && "SP Offset not in bounds.");
  } else {
    if (FrameIndex >= 0) {
      assert((Offset < 0 && -Offset <= StackSize) &&
             "FP Offset not in bounds.");
    }
  }
  ReplaceFrameIndex(II, TII, Reg, getFrameRegister(MF), Offset, StackSize,
                    ObjSize, RS, SPAdj);
}

unsigned ARCRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  const ARCFrameLowering *TFI = getFrameLowering(MF);
  return TFI->hasFP(MF) ? ARC::FP : ARC::SP;
}

const uint32_t *
ARCRegisterInfo::getCallPreservedMask(const MachineFunction &MF,
                                      CallingConv::ID CC) const {
  return CSR_ARC_RegMask;
}
