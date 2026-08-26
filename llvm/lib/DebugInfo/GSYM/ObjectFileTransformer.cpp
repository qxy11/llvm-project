//===- ObjectFileTransformer.cpp --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/DebugInfo/GSYM/GsymCreator.h"
#include "llvm/DebugInfo/GSYM/ObjectFileTransformer.h"
#include "llvm/DebugInfo/GSYM/OutputAggregator.h"

using namespace llvm;
using namespace gsym;

constexpr uint32_t NT_GNU_BUILD_ID_TAG = 0x03;

static std::vector<uint8_t> getUUID(const object::ObjectFile &Obj) {
  // Extract the UUID from the object file
  std::vector<uint8_t> UUID;
  if (auto *MachO = dyn_cast<object::MachOObjectFile>(&Obj)) {
    const ArrayRef<uint8_t> MachUUID = MachO->getUuid();
    if (!MachUUID.empty())
      UUID.assign(MachUUID.data(), MachUUID.data() + MachUUID.size());
  } else if (isa<object::ELFObjectFileBase>(&Obj)) {
    const StringRef GNUBuildID(".note.gnu.build-id");
    for (const object::SectionRef &Sect : Obj.sections()) {
      Expected<StringRef> SectNameOrErr = Sect.getName();
      if (!SectNameOrErr) {
        consumeError(SectNameOrErr.takeError());
        continue;
      }
      StringRef SectName(*SectNameOrErr);
      if (SectName != GNUBuildID)
        continue;
      StringRef BuildIDData;
      Expected<StringRef> E = Sect.getContents();
      if (E)
        BuildIDData = *E;
      else {
        consumeError(E.takeError());
        continue;
      }
      DataExtractor Decoder(BuildIDData, Obj.makeTriple().isLittleEndian());
      uint64_t Offset = 0;
      const uint32_t NameSize = Decoder.getU32(&Offset);
      const uint32_t PayloadSize = Decoder.getU32(&Offset);
      const uint32_t PayloadType = Decoder.getU32(&Offset);
      StringRef Name(Decoder.getFixedLengthString(&Offset, NameSize));
      if (Name == "GNU" && PayloadType == NT_GNU_BUILD_ID_TAG) {
        Offset = alignTo(Offset, 4);
        StringRef UUIDBytes(Decoder.getBytes(&Offset, PayloadSize));
        if (!UUIDBytes.empty()) {
          auto Ptr = reinterpret_cast<const uint8_t *>(UUIDBytes.data());
          UUID.assign(Ptr, Ptr + UUIDBytes.size());
        }
      }
    }
  }
  return UUID;
}

/// Add a FunctionInfo to \a Gsym for every function symbol in \a Obj's symbol
/// table that falls inside the text ranges \a Gsym knows about.
///
/// \param CopyStrings If true, symbol names are copied into the GSYM string
///        table. This is required when \a Obj's backing buffer does not outlive
///        the conversion, as is the case for the object decompressed out of
///        .gnu_debugdata.
static llvm::Error loadSymbolTable(const object::ObjectFile &Obj,
                                   OutputAggregator &Out, GsymCreator &Gsym,
                                   bool CopyStrings) {
  using namespace llvm::object;

  const bool IsMachO = isa<MachOObjectFile>(&Obj);
  const bool IsELF = isa<ELFObjectFileBase>(&Obj);

  for (const object::SymbolRef &Sym : Obj.symbols()) {
    Expected<SymbolRef::Type> SymType = Sym.getType();
    if (!SymType) {
      consumeError(SymType.takeError());
      continue;
    }
    Expected<uint64_t> AddrOrErr = Sym.getValue();
    if (!AddrOrErr)
      // TODO: Test this error.
      return AddrOrErr.takeError();

    if (SymType.get() != SymbolRef::Type::ST_Function ||
        !Gsym.IsValidTextAddress(*AddrOrErr))
      continue;
    // Function size for MachO files will be 0
    const uint64_t size = IsELF ? ELFSymbolRef(Sym).getSize() : 0;
    Expected<StringRef> Name = Sym.getName();
    if (!Name) {
      if (Out.GetOS())
        logAllUnhandledErrors(Name.takeError(), *Out.GetOS(),
                              "ObjectFileTransformer: ");
      else
        consumeError(Name.takeError());
      continue;
    }
    // Remove the leading '_' character in any symbol names if there is one
    // for mach-o files.
    if (IsMachO)
      Name->consume_front("_");
    Gsym.addFunctionInfo(
        FunctionInfo(*AddrOrErr, size, Gsym.insertString(*Name, CopyStrings)));
  }
  return Error::success();
}

llvm::Error ObjectFileTransformer::convert(const object::ObjectFile &Obj,
                                           OutputAggregator &Out,
                                           GsymCreator &Gsym) {
  using namespace llvm::object;

  // Read build ID.
  Gsym.setUUID(getUUID(Obj));

  // Parse the symbol table.
  size_t NumBefore = Gsym.getNumFunctionInfos();
  // The main object's buffer outlives the conversion, so its names can be
  // referenced in place.
  if (Error E = loadSymbolTable(Obj, Out, Gsym, /*CopyStrings=*/false))
    return E;
  size_t FunctionsAddedCount = Gsym.getNumFunctionInfos() - NumBefore;
  if (Out.GetOS())
    *Out.GetOS() << "Loaded " << FunctionsAddedCount
                 << " functions from symbol table.\n";

  // A stripped ELF binary may carry MiniDebugInfo: an xz-compressed ELF object
  // in .gnu_debugdata whose .symtab holds the function symbols that were
  // stripped out of the main object. Those symbols are the only source of
  // names for non-exported functions, so pull them in as well.
  const auto *ELFObj = dyn_cast<ELFObjectFileBase>(&Obj);
  if (!ELFObj || !ELFObj->hasGnuDebugDataSection())
    return Error::success();

  Expected<OwningBinary<ObjectFile>> DebugDataObj =
      ELFObj->getGnuDebugDataObjectFile();
  if (!DebugDataObj) {
    // MiniDebugInfo is a bonus, not a requirement: warn and keep the symbols we
    // already have rather than failing the whole conversion. Stringify the
    // error eagerly, since Report() only runs the callback when it has a
    // stream to write to and the error must be consumed either way.
    std::string ErrMsg = toString(DebugDataObj.takeError());
    Out.Report(
        "Failed to load the .gnu_debugdata section", [&](raw_ostream &OS) {
          OS << "warning: unable to read the .gnu_debugdata section: " << ErrMsg
             << "\n";
        });
    return Error::success();
  }

  NumBefore = Gsym.getNumFunctionInfos();
  // The decompressed buffer dies with DebugDataObj at the end of this function,
  // so these names must be copied into the string table.
  if (Error E = loadSymbolTable(*DebugDataObj->getBinary(), Out, Gsym,
                                /*CopyStrings=*/true))
    return E;
  FunctionsAddedCount = Gsym.getNumFunctionInfos() - NumBefore;
  if (Out.GetOS())
    *Out.GetOS() << "Loaded " << FunctionsAddedCount
                 << " functions from .gnu_debugdata symbol table.\n";
  return Error::success();
}
