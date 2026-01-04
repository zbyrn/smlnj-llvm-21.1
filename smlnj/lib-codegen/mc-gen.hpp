/// \file mc-gen.hpp
///
/// \copyright 2023 The Fellowship of SML/NJ (https://smlnj.org)
/// All rights reserved.
///
/// \brief Wrapper class for the low-level machine-specific parts of the code generator
///
/// \author John Reppy
///

#ifndef _MC_GEN_HPP_
#define _MC_GEN_HPP_

#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/IR/LegacyPassManager.h" /* needed for code gen */

namespace smlnj {
namespace cfgcg {

class MCGen {
  public:

    MCGen (TargetInfo const *target);
    MCGen (TargetInfo const *target, std::optional<std::string_view> pipeline);
    ~MCGen ();

    /// per-module initialization
    void beginModule (llvm::Module *module);

    /// per-module finalization
    void endModule ();

    /// run the per-function optimizations over the functions of the module
    void optimize (llvm::Module *module);

    /// compile the module's code to an in-memory object file
    std::unique_ptr<class ObjectFile> emitMC (llvm::Module *module);

    /// dump the code for the module to an output file; the file type should be
    /// either `AssemblyFile` or `ObjectFile`
    void emitFile (
        llvm::Module *module,
        std::string const &outFile,
        llvm::CodeGenFileType fileType);

  private:
    TargetInfo const *_tgtInfo;
    std::unique_ptr<llvm::TargetMachine> _tgtMachine;

  // analysis and optimization managers
    // NOTE: the analysis managers must be declared in the following order
    // to ensure they are destroyed in the correct order. Function Analysis
    // Manager may refer to results stored in Loop Analysis Manager. For memory
    // safety, _lam must outlive _fam. Because we only perform function-level
    // optimizations, _mam should not refer to CGSCC-level analyses. Thus, a
    // placeholder CGSCC Analysis Manager is locally constructed and discarded
    // in MCGen's constructor. If this fact changes in the future, a _cgam field
    // must be inserted between _fam and _mam below.
    llvm::LoopAnalysisManager _lam;
    llvm::FunctionAnalysisManager _fam;
    llvm::ModuleAnalysisManager _mam;

    llvm::ModulePassManager _pm;
    llvm::PassBuilder *_pb;

};

} // namespace cfgcg
} // namespace smlnj

#endif // !_MC_GEN_HPP_
