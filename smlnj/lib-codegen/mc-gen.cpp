/// \file mc-gen.cpp
///
/// \copyright 2023 The Fellowship of SML/NJ (https://smlnj.org)
/// All rights reserved.
///
/// \brief Wrapper for the low-level machine-specific parts of the code generator
///
/// \author John Reppy
///

#include "objfile-stream.hpp"
#include "target-info.hpp"
#include "mc-gen.hpp"
#include "context.hpp"
#include "object-file.hpp"

#include "llvm/IR/LegacyPassManager.h" /* needed for code gen */
#include "llvm/IR/Verifier.h" /* needed for code gen */
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/AlignmentFromAssumptions.h"
#include "llvm/Transforms/Scalar/CorrelatedValuePropagation.h"
#include "llvm/Transforms/Scalar/ConstraintElimination.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/BDCE.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Transforms/Scalar/DivRemPairs.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/Float2Int.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/InferAlignment.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/LICM.h"
#include "llvm/Transforms/Scalar/LoopDeletion.h"
#include "llvm/Transforms/Scalar/LoopDistribute.h"
#include "llvm/Transforms/Scalar/LoopInstSimplify.h"
#include "llvm/Transforms/Scalar/LoopLoadElimination.h"
#include "llvm/Transforms/Scalar/LoopRotation.h"
#include "llvm/Transforms/Scalar/LoopSimplifyCFG.h"
#include "llvm/Transforms/Scalar/LoopSink.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/Transforms/Scalar/LowerExpectIntrinsic.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/SimpleLoopUnswitch.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/InjectTLIMappings.h"
#include "llvm/Transforms/Vectorize/LoopVectorize.h"
#include "llvm/Transforms/Vectorize/SLPVectorizer.h"
#include "llvm/Transforms/Vectorize/VectorCombine.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace smlnj {
namespace cfgcg {

static const char *DEFAULT_PIPELINE = "function("
     "lower-expect,"
     "simplifycfg,"
     "instcombine<no-verify-fixpoint>,"
     "reassociate,"
     "early-cse,"
     "gvn,"
     "sccp,"
     "dce,"
     "simplifycfg,"
     "instcombine<no-verify-fixpoint>,"
     "simplifycfg<switch-to-lookup>"
     ")";

llvm::FunctionPassManager buildDefaultOptimizationPipeline()
{
    using namespace llvm;

    const OptimizationLevel Level = OptimizationLevel::O2;
    const ThinOrFullLTOPhase LTOPhase = ThinOrFullLTOPhase::None;

    FunctionPassManager OptimizePM;

    // -- Constructs the core canonicalization and simplification pipeline
    // Lower llvm.expect to metadata
    OptimizePM.addPass(LowerExpectIntrinsicPass());
    OptimizePM.addPass(SimplifyCFGPass());
    // Break apart aggregates into scalars
    OptimizePM.addPass(SROAPass(SROAOptions::ModifyCFG));
    OptimizePM.addPass(InstCombinePass());
    OptimizePM.addPass(ReassociatePass());
    OptimizePM.addPass(EarlyCSEPass(/*UseMemorySSA=*/true));

    // -- Simplification
    OptimizePM.addPass(GVNPass());  // global value numbering
    OptimizePM.addPass(SCCPPass()); // sparse conditional constant propagation
    OptimizePM.addPass(DCEPass());  // Dead-code elim with bit-tracking
    // Demote floating point numbers to integers where possible. In the case of
    // SML/NJ, since most floating point operations are done as external function
    // calls, I'm not sure how much this pass is beneficial.
    // OptimizePM.addPass(Float2IntPass());
    OptimizePM.addPass(CorrelatedValuePropagationPass());
    OptimizePM.addPass(ConstraintEliminationPass());
    OptimizePM.addPass(InstCombinePass());
    OptimizePM.addPass(AggressiveInstCombinePass());
    OptimizePM.addPass(SimplifyCFGPass());
    OptimizePM.addPass(InstCombinePass());

    // // -- Now adds the optimization passes.

    // // The next step LLVM performs is LowerConstantInstrinsicsPass(), which lowers
    // // `objectsize` and `is.constant` intrinsic calls. As far as I know, we don't
    // // use those intrinsics.
    // // 
    // // OptimizePM.addPass(llvm::LowerConstantIntrinsicsPass());

    // -- Optimize the loop execution.
    LoopPassManager LPM;
    LPM.addPass(LoopInstSimplifyPass());
    LPM.addPass(LoopSimplifyCFGPass());
    // Rotate loops that may have been un-rotated.
    // LPM.addPass(LoopRotatePass());
    LPM.addPass(LICMPass(LICMOptions()));
    LPM.addPass(SimpleLoopUnswitchPass());
    // Some loops may have become dead by now. Try to delete them.
    LPM.addPass(LoopDeletionPass());

    // The following line, when uncommented, causes memory error.
    OptimizePM.addPass(createFunctionToLoopPassAdaptor(std::move(LPM), /*UseMemorySSA=*/true));
    // OptimizePM.addPass(VerifierPass());

    // -- Vectorization.

    // Populates the VFABI attribute with the scalar-to-vector mappings from the
    // TargetLibraryInfo
    OptimizePM.addPass(InjectTLIMappings());

    // Add the vectorization transformation passes
    OptimizePM.addPass(LoopVectorizePass());
    OptimizePM.addPass(InferAlignmentPass());
    OptimizePM.addPass(LoopLoadEliminationPass());
    OptimizePM.addPass(InstCombinePass());
    OptimizePM.addPass(SimplifyCFGPass());
    OptimizePM.addPass(SLPVectorizerPass());
    OptimizePM.addPass(VectorCombinePass());
    OptimizePM.addPass(InstCombinePass());
    OptimizePM.addPass(LoopUnrollPass());
    // Because we have LICM and SimplifyCFG scheduled afterwards, we
    // may allow SROA to modify the CFG.
    OptimizePM.addPass(SROAPass(SROAOptions::PreserveCFG));
    OptimizePM.addPass(InferAlignmentPass());
    OptimizePM.addPass(InstCombinePass());
    OptimizePM.addPass(createFunctionToLoopPassAdaptor(LICMPass(LICMOptions()), /*UseMemorySSA=*/true));
    OptimizePM.addPass(AlignmentFromAssumptionsPass());

    // Sinks instructions hoisted by LICM, which serves as a 
    // canonicalization pass that enables other optimizations.
    // OptimizePM.addPass(LoopSinkPass());

    // Clean up LCSSA form before generating code.
    OptimizePM.addPass(InstSimplifyPass());

    // Hoists div/mod operators. It should run after other sink/hoist
    // passes to avoid re-sinking, but before SimplifyCFG because it 
    // can allow flattening of blocks.
    // OptimizePM.addPass(DivRemPairsPass());

    OptimizePM.addPass(SCCPPass());
    OptimizePM.addPass(ADCEPass());
    OptimizePM.addPass(SimplifyCFGPass());
    OptimizePM.addPass(InstCombinePass());
    // Loop passes might have resulted in single-entry-single-exit or empty
    // blocks. Clean up the CFG.
    OptimizePM.addPass(
        SimplifyCFGPass(SimplifyCFGOptions()
                            // .convertSwitchRangeToICmp(true)
                            .convertSwitchToLookupTable(true)
                            // .speculateUnpredictables(true)
                            // .hoistLoadsStoresWithCondFaulting(true)
        ));
    OptimizePM.addPass(VerifierPass());

    return OptimizePM;
}

template <typename IRUnitT> static const IRUnitT *unwrapIR(llvm::Any IR) {
  const IRUnitT **IRPtr = llvm::any_cast<const IRUnitT *>(&IR);
  return IRPtr ? *IRPtr : nullptr;
}

void dumpDiff(llvm::StringRef PassName, llvm::Any IR, const llvm::PreservedAnalyses &PA) {
    (void) PA;
    const char *Dir = std::getenv("LLVM_DIFF_DUMP");
    if (Dir == nullptr) {
        return;
    }

    if (const auto *F = unwrapIR<llvm::Function>(IR)) {
        std::filesystem::path Path(Dir);
        // Path /= F->getName().str();

        std::error_code EC;
        std::filesystem::create_directories(Path, EC);
        if (EC) {
            llvm::errs() << "Failed to create directory \"" << Path << "\": "
                         << EC.message() << '\n';
            return;
        }
        
        Path /= PassName.str() + ".ll";

        EC.clear();
        llvm::raw_fd_ostream Out(Path.native(), EC, llvm::sys::fs::OpenFlags::OF_Append);
        if (EC) {
            llvm::errs() << "Failed to open file\"" << Path << "\": "
                         << EC.message() << '\n';
            return;
        }
        F->print(Out, nullptr);
    }
}

MCGen::MCGen (TargetInfo const *info, std::optional<std::string_view> pipeline)
  : _tgtInfo(info), _tgtMachine(nullptr), _lam(), _fam(), _mam(), _pb(nullptr)
{
  // get the LLVM target triple
    llvm::Triple triple = info->getTriple();

  // lookup the target in the registry using the triple's string representation
    std::string errMsg;
    auto *target = llvm::TargetRegistry::lookupTarget(triple.str(), errMsg);
    if (target == nullptr) {
	std::cerr << "**** Fatal error: unable to find target for \""
	    << info->name << "\"\n";
	std::cerr << "    [" << errMsg << "]\n";
        assert(false);
    }

    llvm::TargetOptions tgtOptions;

  // floating-point target options
    tgtOptions.setFP32DenormalMode (llvm::DenormalMode::getIEEE());
    tgtOptions.setFPDenormalMode (llvm::DenormalMode::getIEEE());

// TODO: enable tgtOptions.EnableFastISel?

  // make sure that tail calls are optimized
  /* It turns out that setting the GuaranteedTailCallOpt flag to true causes
   * a bug with non-tail JWA calls (the bug is a bogus stack adjustment after
   * the call).  Fortunately, our tail calls get properly optimized even
   * without that flag being set.
   */
//    tgtOpts.GuaranteedTailCallOpt = true;

// see include/llvm/Support/*Parser.def for the various CPU and feature names
// that are recognized
    std::unique_ptr<llvm::TargetMachine> tgtMachine(target->createTargetMachine(
	triple,
	"generic",		/* CPU name */
	"",			/* features string */
	tgtOptions,
	llvm::Reloc::PIC_,
	std::optional<llvm::CodeModel::Model>(),
	llvm::CodeGenOptLevel::Less));

    if (!tgtMachine) {
	std::cerr << "**** Fatal error: unable to create target machine\n";
        assert(false);
    }

    this->_tgtMachine = std::move(tgtMachine);

    // Create the new pass manager builder.
    this->_pb = new llvm::PassBuilder(
        this->_tgtMachine.get(),
        llvm::PipelineTuningOptions(),
        std::nullopt,
        new llvm::PassInstrumentationCallbacks()
    );
    this->_pb->getPassInstrumentationCallbacks()->registerAfterPassCallback(dumpDiff);

    // A CGSCCAnalysisManager is constructed and discarded because we do not
    // perform CGSCC-level analysis. See the header file for details.
    llvm::CGSCCAnalysisManager cgam;
    this->_pb->registerModuleAnalyses(this->_mam);
    this->_pb->registerCGSCCAnalyses(cgam);
    this->_pb->registerFunctionAnalyses(this->_fam);
    this->_pb->registerLoopAnalyses(this->_lam);
    this->_pb->crossRegisterProxies(this->_lam, this->_fam, cgam, this->_mam);

    // set up the optimization passes
    llvm::FunctionPassManager fpm;
    if (pipeline.has_value()) {
        assert(false && "disabled");
        // if (auto err = this->_pb->parsePassPipeline(fpm, pipeline.value())) {
        //     llvm::errs() << "Pipeline Error: " << err << "\n";
        //     assert(false);
        // }
    } else {
        fpm = buildDefaultOptimizationPipeline();
        // fpm = this->_pb->buildFunctionSimplificationPipeline(
        //     llvm::OptimizationLevel::O2, llvm::ThinOrFullLTOPhase::None);
        // fpm.addPass(llvm::LowerExpectIntrinsicPass());      // -lower-expect
        // fpm.addPass(llvm::SimplifyCFGPass());               // -simplifycfg
        // fpm.addPass(llvm::InstCombinePass());               // -instcombine
        // fpm.addPass(llvm::ReassociatePass());               // -reassociate
        // fpm.addPass(llvm::EarlyCSEPass(false));             // -early-cse
        // fpm.addPass(llvm::GVNPass());                       // -gvn
        // fpm.addPass(llvm::SCCPPass());                      // -sccp
        // fpm.addPass(llvm::DCEPass());                       // -dce
        // fpm.addPass(llvm::SimplifyCFGPass());               // -simplifycfg
        // fpm.addPass(llvm::InstCombinePass());               // -instcombine
        // // for the last simplification pass, we want to convert switches to jump tables
        // llvm::SimplifyCFGOptions opts;
        // opts.ConvertSwitchToLookupTable = true;
        // fpm.addPass(llvm::SimplifyCFGPass(opts));           // -simplifycfg
        // this->_pm.addPass (llvm::createModuleToFunctionPassAdaptor(
        //     this->_pb->buildFunctionSimplificationPipeline(llvm::OptimizationLevel::O2, llvm::ThinOrFullLTOPhase::None)));
    }

    this->_pm.addPass (llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
    // this->_pm.printPipeline(llvm::dbgs(), [](llvm::StringRef s) { return s; });
} // MCGen constructor


MCGen::MCGen(TargetInfo const *info) : MCGen(info, std::nullopt)
{ }

MCGen::~MCGen ()
{
    if (this->_pb != nullptr) {
        delete this->_pb;
    }

}

void MCGen::beginModule (llvm::Module *module)
{
  // tell the module about the target machine
    module->setTargetTriple(this->_tgtMachine->getTargetTriple());
    module->setDataLayout(this->_tgtMachine->createDataLayout());

} // MCGen::beginModule

void MCGen::endModule () { }

void MCGen::optimize (llvm::Module *module)
{
    // std::cerr << "Running passes:\n";

    // this->_pm.printPipeline(llvm::errs(), [](llvm::StringRef s) { return s; });
  // run the function optimizations over every function
    // std::cerr << "** Before All:\n";
    // module->print(llvm::errs(), nullptr);
    //
    // auto *PrintAfterAll = static_cast<llvm::cl::opt<bool> *>(llvm::cl::getRegisteredOptions()["print-after-all"]);
    // *PrintAfterAll = true;

    this->_pm.run (*module, this->_mam);
    // std::cerr << "** After All:\n";
    // module->print(llvm::errs(), nullptr);
    // *PrintAfterAll = false;
}

// compile the code into the code buffer's object-file backing store.
//
// adopted from SimpleCompiler::operator() (lib/ExecutionEngine/Orc/CompileUtils.cpp)
//
std::unique_ptr<ObjectFile> MCGen::emitMC (llvm::Module *module)
{
    ObjfileStream objStrm;
    llvm::legacy::PassManager pass;
    llvm::MCContext *ctx; /* result parameter */
    if (this->_tgtMachine->addPassesToEmitMC(pass, ctx, objStrm)) {
        llvm::report_fatal_error ("unable to add pass to generate code", true);
    }
    pass.run (*module);

    return std::make_unique<ObjectFile>(objStrm);

}

void MCGen::emitFile (
    llvm::Module *module,
    std::string const &outFile,
    llvm::CodeGenFileType fileType)
{
    std::error_code EC;
    llvm::raw_fd_ostream outStrm(outFile, EC, llvm::sys::fs::OF_None);
    if (EC) {
        llvm::errs() << "unable to open output file '" << outFile << "'\n";
        return;
    }

    llvm::legacy::PassManager pass;
    if (this->_tgtMachine->addPassesToEmitFile(pass, outStrm, nullptr, fileType)) {
        llvm::errs() << "unable to add pass to generate '" << outFile << "'\n";
        return;
    }

    pass.run(*module);

    outStrm.flush();

}

} // namespace cfgcg
} // namespace smlnj
