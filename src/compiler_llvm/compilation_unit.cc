/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "compilation_unit.h"

#include "constants.h"
#include "ir_builder.h"
#include "logging.h"

#include <llvm/ADT/OwningPtr.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/ADT/Triple.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/DebugInfo.h>
#include <llvm/Analysis/LoopPass.h>
#include <llvm/Analysis/RegionPass.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/Assembly/PrintModulePass.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/CallGraphSCCPass.h>
#include <llvm/DerivedTypes.h>
#include <llvm/LLVMContext.h>
#include <llvm/LinkAllPasses.h>
#include <llvm/LinkAllVMCore.h>
#include <llvm/Module.h>
#include <llvm/PassManager.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PassNameParser.h>
#include <llvm/Support/PluginLoader.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SystemUtils.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetData.h>
#include <llvm/Target/TargetLibraryInfo.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include <string>

namespace art {
namespace compiler_llvm {

llvm::Module* makeLLVMModuleContents(llvm::Module* module);


CompilationUnit::CompilationUnit(InstructionSet insn_set)
: insn_set_(insn_set), context_(new llvm::LLVMContext()), mem_usage_(0) {

  // Create the module and include the runtime function declaration
  module_ = new llvm::Module("art", *context_);
  makeLLVMModuleContents(module_);

  // Create IRBuilder
  irb_.reset(new IRBuilder(*context_, *module_));
}


CompilationUnit::~CompilationUnit() {
}


bool CompilationUnit::WriteBitcodeToFile() {
  std::string errmsg;

  llvm::OwningPtr<llvm::tool_output_file> out_file(
    new llvm::tool_output_file(bitcode_filename_.c_str(), errmsg,
                               llvm::raw_fd_ostream::F_Binary));


  if (!errmsg.empty()) {
    LOG(ERROR) << "Failed to create bitcode output file: " << errmsg;
    return false;
  }

  llvm::WriteBitcodeToFile(module_, out_file->os());
  out_file->keep();

  return true;
}


bool CompilationUnit::Materialize() {
  // Initialize LLVM optimization passes
  llvm::PassRegistry &Registry = *llvm::PassRegistry::getPassRegistry();

  llvm::initializeCore(Registry);
  llvm::initializeScalarOpts(Registry);
  llvm::initializeIPO(Registry);
  llvm::initializeAnalysis(Registry);
  llvm::initializeIPA(Registry);
  llvm::initializeTransformUtils(Registry);
  llvm::initializeInstCombine(Registry);
  llvm::initializeInstrumentation(Registry);
  llvm::initializeTarget(Registry);

  // Lookup the LLVM target
  char const* target_triple = NULL;
  char const* target_attr = NULL;

  if (insn_set_ == kThumb2) {
    target_triple = "thumb-none-linux-gnueabi";
    target_attr = "+thumb2,+neon,+neonfp,+vfp3";
  } else if (insn_set_ == kArm) {
    target_triple = "armv7-none-linux-gnueabi";
    target_attr = "+v7,+neon,+neonfp,+vfp3";
  } else if (insn_set_ == kX86) {
    target_triple = "i386-pc-linux-gnu";
    target_attr = "";
  // } else if (insn_set_ == kMips) {
  //  target_triple = "mipsel-unknown-linux";
  //  target_attr = "";
  } else {
    LOG(FATAL) << "Unknown instruction set: " << insn_set_;
  }

  std::string errmsg;
  llvm::Target const* target =
    llvm::TargetRegistry::lookupTarget(target_triple, errmsg);

  CHECK(target != NULL) << errmsg;

  // Target options
  llvm::TargetOptions target_options;

  target_options.FloatABIType = llvm::FloatABI::Soft;
  target_options.UseSoftFloat = false;
  target_options.NoFramePointerElim = true;
  target_options.NoFramePointerElimNonLeaf = true;

  // Create the llvm::TargetMachine
  llvm::TargetMachine* target_machine =
    target->createTargetMachine(target_triple, "", target_attr, target_options,
                                llvm::Reloc::Static, llvm::CodeModel::Small,
                                llvm::CodeGenOpt::Aggressive);

  CHECK(target_machine != NULL) << "Failed to create target machine";


  // Add target data
  llvm::TargetData const* target_data = target_machine->getTargetData();

  // PassManager for code generation passes
  llvm::PassManager pm;
  pm.add(new llvm::TargetData(*target_data));

  // FunctionPassManager for optimization pass
  llvm::FunctionPassManager fpm(module_);
  fpm.add(new llvm::TargetData(*target_data));

  // Add optimization pass
  llvm::PassManagerBuilder pm_builder;
  pm_builder.Inliner = NULL; // TODO: add some inline in the future
  pm_builder.OptLevel = 3;
  pm_builder.DisableSimplifyLibCalls = 1;
  pm_builder.populateModulePassManager(pm);
  pm_builder.populateFunctionPassManager(fpm);

  // Add passes to emit file
  llvm::OwningPtr<llvm::tool_output_file> out_file(
    new llvm::tool_output_file(elf_filename_.c_str(), errmsg,
                               llvm::raw_fd_ostream::F_Binary));

  llvm::formatted_raw_ostream formatted_os(out_file->os(), false);

  // Ask the target to add backend passes as necessary.
  if (target_machine->addPassesToEmitFile(pm,
                                          formatted_os,
                                          llvm::TargetMachine::CGFT_ObjectFile,
                                          true)) {
    LOG(FATAL) << "Unable to generate ELF for this target";
    return false;
  }

  // Run the per-function optimization
  fpm.doInitialization();
  for (llvm::Module::iterator F = module_->begin(), E = module_->end();
       F != E; ++F) {
    fpm.run(*F);
  }
  fpm.doFinalization();
  LOG(INFO) << "Intraprocedural optimization finished!";

  // Run the code generation passes
  pm.run(*module_);
  LOG(INFO) << "Code generation finished!";

  out_file->keep();
  LOG(DEBUG) << "ELF: " << elf_filename_ << " (done)";

  return true;
}


void CompilationUnit::Finalize() {
  context_.reset(NULL);
  irb_.reset(NULL);
  module_ = NULL;
}

} // namespace compiler_llvm
} // namespace art
