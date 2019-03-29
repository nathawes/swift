//===-- ParseableInterfaceModuleLoader.cpp - Loads .swiftinterface files --===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "textual-module-interface"
#include "swift/Frontend/ParseableInterfaceModuleLoader.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/FileSystem.h"
#include "swift/AST/Module.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/Basic/Lazy.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/ParseableInterfaceSupport.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/Serialization/ModuleFormat.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/Serialization/Validation.h"
#include "clang/Basic/Module.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/xxhash.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/YAMLTraits.h"

using namespace swift;
using FileDependency = SerializationOptions::FileDependency;

/// Extract the specified-or-defaulted -module-cache-path that winds up in
/// the clang importer, for reuse as the .swiftmodule cache path when
/// building a ParseableInterfaceModuleLoader.
std::string
swift::getModuleCachePathFromClang(const clang::CompilerInstance &Clang) {
  if (!Clang.hasPreprocessor())
    return "";
  std::string SpecificModuleCachePath = Clang.getPreprocessor()
    .getHeaderSearchInfo()
    .getModuleCachePath();

  // The returned-from-clang module cache path includes a suffix directory
  // that is specific to the clang version and invocation; we want the
  // directory above that.
  return llvm::sys::path::parent_path(SpecificModuleCachePath);
}

#pragma mark - Forwarding Modules

namespace {

/// Describes a "forwarding module", that is, a .swiftmodule that's actually
/// a YAML file inside, pointing to a the original .swiftmodule but describing
/// a different dependency resolution strategy.
struct ForwardingModule {
  /// The path to the original .swiftmodule in the prebuilt cache.
  std::string underlyingModulePath;

  /// Describes a set of file-based dependencies with their size and
  /// modification time stored. This is slightly different from
  /// \c SerializationOptions::FileDependency, because this type needs to be
  /// serializable to and from YAML.
  struct Dependency {
    std::string path;
    uint64_t size;
    uint64_t lastModificationTime;
  };
  std::vector<Dependency> dependencies;
  unsigned version = 1;

  ForwardingModule() = default;
  ForwardingModule(StringRef underlyingModulePath)
  : underlyingModulePath(underlyingModulePath) {}

  /// Loads the contents of the forwarding module whose contents lie in
  /// the provided buffer, and returns a new \c ForwardingModule, or an error
  /// if the YAML could not be parsed.
  static llvm::ErrorOr<ForwardingModule> load(const llvm::MemoryBuffer &buf);

  /// Adds a given dependency to the dependencies list.
  void addDependency(StringRef path, uint64_t size, uint64_t modTime) {
    dependencies.push_back({path.str(), size, modTime});
  }
};

} // end anonymous namespace

#pragma mark - YAML Serialization

namespace llvm {
  namespace yaml {
    template <>
    struct MappingTraits<ForwardingModule::Dependency> {
      static void mapping(IO &io, ForwardingModule::Dependency &dep) {
        io.mapRequired("mtime", dep.lastModificationTime);
        io.mapRequired("path", dep.path);
        io.mapRequired("size", dep.size);
      }
    };

    template <>
    struct SequenceElementTraits<ForwardingModule::Dependency> {
      static const bool flow = false;
    };

    template <>
    struct MappingTraits<ForwardingModule> {
      static void mapping(IO &io, ForwardingModule &module) {
        io.mapRequired("path", module.underlyingModulePath);
        io.mapRequired("dependencies", module.dependencies);
        io.mapRequired("version", module.version);
      }
    };
  }
} // end namespace llvm

llvm::ErrorOr<ForwardingModule>
ForwardingModule::load(const llvm::MemoryBuffer &buf) {
  llvm::yaml::Input yamlIn(buf.getBuffer());
  ForwardingModule fwd;
  yamlIn >> fwd;
  if (yamlIn.error())
    return yamlIn.error();

  // We only currently support Version 1 of the forwarding module format.
  if (fwd.version != 1)
    return std::make_error_code(std::errc::not_supported);
  return std::move(fwd);
}

#pragma mark - Module Discovery

namespace {

/// The result of a search for a module either alongside an interface, in the
/// module cache, or in the prebuilt module cache.
class DiscoveredModule {
  /// The kind of module we've found.
  enum class Kind {
    /// A module that's either alongside the swiftinterface or in the
    /// module cache.
    Normal,

    /// A module that resides in the prebuilt cache, and has hash-based
    /// dependencies.
    Prebuilt,

    /// A 'forwarded' module. This is a module in the prebuilt cache, but whose
    /// dependencies live in a forwarding module.
    /// \sa ForwardingModule.
    Forwarded
  };

  /// The kind of module that's been discovered.
  const Kind kind;

  DiscoveredModule(StringRef path, Kind kind,
    std::unique_ptr<llvm::MemoryBuffer> moduleBuffer)
    : kind(kind), moduleBuffer(std::move(moduleBuffer)), path(path) {}

public:
  /// The contents of the .swiftmodule, if we've read it while validating
  /// dependencies.
  std::unique_ptr<llvm::MemoryBuffer> moduleBuffer;

  /// The path to the discovered serialized .swiftmodule on disk.
  const std::string path;

  /// Creates a \c Normal discovered module.
  static DiscoveredModule normal(StringRef path,
      std::unique_ptr<llvm::MemoryBuffer> moduleBuffer) {
    return { path, Kind::Normal, std::move(moduleBuffer) };
  }

  /// Creates a \c Prebuilt discovered module.
  static DiscoveredModule prebuilt(
      StringRef path, std::unique_ptr<llvm::MemoryBuffer> moduleBuffer) {
    return { path, Kind::Prebuilt, std::move(moduleBuffer) };
  }

  /// Creates a \c Forwarded discovered module, whose dependencies have been
  /// externally validated by a \c ForwardingModule.
  static DiscoveredModule forwarded(
      StringRef path, std::unique_ptr<llvm::MemoryBuffer> moduleBuffer) {
    return { path, Kind::Forwarded, std::move(moduleBuffer) };
  }

  bool isNormal() const { return kind == Kind::Normal; }
  bool isPrebuilt() const { return kind == Kind::Prebuilt; }
  bool isForwarded() const { return kind == Kind::Forwarded; }
};

} // end anonymous namespace

#pragma mark - Common utilities

namespace path = llvm::sys::path;

static bool serializedASTLooksValid(const llvm::MemoryBuffer &buf) {
  auto VI = serialization::validateSerializedAST(buf.getBuffer());
  return VI.status == serialization::Status::Valid;
}

static std::unique_ptr<llvm::MemoryBuffer> getBufferOfDependency(
  llvm::vfs::FileSystem &fs, StringRef depPath, StringRef interfacePath,
  DiagnosticEngine &diags, SourceLoc diagnosticLoc) {
  auto depBuf = fs.getBufferForFile(depPath, /*FileSize=*/-1,
                                    /*RequiresNullTerminator=*/false);
  if (!depBuf) {
    diags.diagnose(diagnosticLoc,
                   diag::missing_dependency_of_parseable_module_interface,
                   depPath, interfacePath, depBuf.getError().message());
    return nullptr;
  }
  return std::move(depBuf.get());
}

static Optional<llvm::vfs::Status> getStatusOfDependency(
  llvm::vfs::FileSystem &fs, StringRef depPath, StringRef interfacePath,
  DiagnosticEngine &diags, SourceLoc diagnosticLoc) {
  auto status = fs.status(depPath);
  if (!status) {
    diags.diagnose(diagnosticLoc,
                   diag::missing_dependency_of_parseable_module_interface,
                   depPath, interfacePath, status.getError().message());
    return None;
  }
  return status.get();
}

#pragma mark - Module Building

/// Builds a parseable module interface into a .swiftmodule at the provided
/// output path.
/// \note Needs to be in the swift namespace so CompilerInvocation can see it.
class swift::ParseableInterfaceBuilder {
  ASTContext &ctx;
  llvm::vfs::FileSystem &fs;
  DiagnosticEngine &diags;
  const StringRef interfacePath;
  const StringRef moduleName;
  const StringRef moduleCachePath;
  const StringRef prebuiltCachePath;
  const bool serializeDependencyHashes;
  const bool trackSystemDependencies;
  const SourceLoc diagnosticLoc;
  DependencyTracker *const dependencyTracker;
  CompilerInvocation subInvocation;

  void configureSubInvocationInputsAndOutputs(StringRef OutPath) {
    auto &SubFEOpts = subInvocation.getFrontendOptions();
    SubFEOpts.RequestedAction = FrontendOptions::ActionType::EmitModuleOnly;
    SubFEOpts.InputsAndOutputs.addPrimaryInputFile(interfacePath);
    SupplementaryOutputPaths SOPs;
    SOPs.ModuleOutputPath = OutPath.str();

    // Pick a primary output path that will cause problems to use.
    StringRef MainOut = "/<unused>";
    SubFEOpts.InputsAndOutputs
      .setMainAndSupplementaryOutputs({MainOut}, {SOPs});
  }

  void configureSubInvocation() {
    auto &SearchPathOpts = ctx.SearchPathOpts;
    auto &LangOpts = ctx.LangOpts;

    // Start with a SubInvocation that copies various state from our
    // invoking ASTContext.
    subInvocation.setImportSearchPaths(SearchPathOpts.ImportSearchPaths);
    subInvocation.setFrameworkSearchPaths(SearchPathOpts.FrameworkSearchPaths);
    subInvocation.setSDKPath(SearchPathOpts.SDKPath);
    subInvocation.setInputKind(InputFileKind::SwiftModuleInterface);
    subInvocation.setRuntimeResourcePath(SearchPathOpts.RuntimeResourcePath);
    subInvocation.setTargetTriple(LangOpts.Target);

    subInvocation.setModuleName(moduleName);
    subInvocation.setClangModuleCachePath(moduleCachePath);
    subInvocation.getFrontendOptions().PrebuiltModuleCachePath =
      prebuiltCachePath;
    subInvocation.getFrontendOptions().TrackSystemDeps = trackSystemDependencies;

    // Respect the detailed-record preprocessor setting of the parent context.
    // This, and the "raw" clang module format it implicitly enables, are
    // required by sourcekitd.
    if (auto *ClangLoader = ctx.getClangModuleLoader()) {
      auto &Opts = ClangLoader->getClangInstance().getPreprocessorOpts();
      if (Opts.DetailedRecord) {
        subInvocation.getClangImporterOptions().DetailedPreprocessingRecord = true;
      }
    }

    // Inhibit warnings from the SubInvocation since we are assuming the user
    // is not in a position to fix them.
    subInvocation.getDiagnosticOptions().SuppressWarnings = true;

    // Inherit this setting down so that it can affect error diagnostics (mostly
    // by making them non-fatal).
    subInvocation.getLangOptions().DebuggerSupport = LangOpts.DebuggerSupport;

    // Disable this; deinitializers always get printed with `@objc` even in
    // modules that don't import Foundation.
    subInvocation.getLangOptions().EnableObjCAttrRequiresFoundation = false;

    // Tell the subinvocation to serialize dependency hashes if asked to do so.
    auto &frontendOpts = subInvocation.getFrontendOptions();
    frontendOpts.SerializeParseableModuleInterfaceDependencyHashes =
      serializeDependencyHashes;
  }

  bool extractSwiftInterfaceVersionAndArgs(
    swift::version::Version &Vers, llvm::StringSaver &SubArgSaver,
    SmallVectorImpl<const char *> &SubArgs) {
    auto FileOrError = swift::vfs::getFileOrSTDIN(fs, interfacePath);
    if (!FileOrError) {
      diags.diagnose(diagnosticLoc, diag::error_open_input_file,
                     interfacePath, FileOrError.getError().message());
      return true;
    }
    auto SB = FileOrError.get()->getBuffer();
    auto VersRe = getSwiftInterfaceFormatVersionRegex();
    auto FlagRe = getSwiftInterfaceModuleFlagsRegex();
    SmallVector<StringRef, 1> VersMatches, FlagMatches;
    if (!VersRe.match(SB, &VersMatches)) {
      diags.diagnose(diagnosticLoc,
                     diag::error_extracting_version_from_parseable_interface);
      return true;
    }
    if (!FlagRe.match(SB, &FlagMatches)) {
      diags.diagnose(diagnosticLoc,
                     diag::error_extracting_flags_from_parseable_interface);
      return true;
    }
    assert(VersMatches.size() == 2);
    assert(FlagMatches.size() == 2);
    Vers = swift::version::Version(VersMatches[1], SourceLoc(), &diags);
    llvm::cl::TokenizeGNUCommandLine(FlagMatches[1], SubArgSaver, SubArgs);
    return false;
  }

  /// Determines if the dependency with the provided path is a swiftmodule in
  /// either the module cache or prebuilt module cache
  bool isCachedModule(StringRef DepName) const {
    if (moduleCachePath.empty() && prebuiltCachePath.empty())
        return false;

    auto Ext = llvm::sys::path::extension(DepName);
    auto Ty = file_types::lookupTypeForExtension(Ext);
    return Ty == file_types::TY_SwiftModuleFile &&
        ((!moduleCachePath.empty() && DepName.startswith(moduleCachePath)) ||
         (!prebuiltCachePath.empty() && DepName.startswith(prebuiltCachePath)));
  }

  /// Populate the provided \p Deps with \c FileDependency entries for all
  /// dependencies \p SubInstance's DependencyTracker recorded while compiling
  /// the module, excepting .swiftmodules in \p moduleCachePath or
  /// \p prebuiltCachePath. Those have _their_ dependencies added instead, both
  /// to avoid having to do recursive scanning when rechecking this dependency
  /// in future and to make the module caches relocatable.
  bool collectDepsForSerialization(CompilerInstance &SubInstance,
                                   SmallVectorImpl<FileDependency> &Deps,
                                   bool IsHashBased) {
    StringRef SDKPath = SubInstance.getASTContext().SearchPathOpts.SDKPath;
    auto DTDeps = SubInstance.getDependencyTracker()->getDependencies();
    SmallVector<StringRef, 16> InitialDepNames(DTDeps.begin(), DTDeps.end());
    InitialDepNames.push_back(interfacePath);
    llvm::StringSet<> AllDepNames;

    for (auto const &DepName : InitialDepNames) {
      // Adjust the paths of dependences in the SDK to be relative to it
      bool IsSDKRelative = false;
      StringRef DepNameToStore = DepName;
      if (SDKPath.size() > 1 && DepName.startswith(SDKPath)) {
        assert(DepName.size() > SDKPath.size() &&
            "should never depend on a directory");
        if (llvm::sys::path::is_separator(DepName[SDKPath.size()])) {
          // Is the DepName something like ${SDKPath}/foo.h"?
          DepNameToStore = DepName.substr(SDKPath.size() + 1);
          IsSDKRelative = true;
        } else if (llvm::sys::path::is_separator(SDKPath.back())) {
          // Is the DepName something like "${SDKPath}foo.h", where SDKPath
          // itself contains a trailing slash?
          DepNameToStore = DepName.substr(SDKPath.size());
          IsSDKRelative = true;
        } else {
          // We have something next to an SDK, like "Foo.sdk.h", that's somehow
          // become a dependency.
        }
      }

      if (AllDepNames.insert(DepName).second && dependencyTracker) {
        dependencyTracker->addDependency(DepName, /*isSystem*/IsSDKRelative);
      }

      /// Lazily load the dependency buffer if we need it. If we're not
      /// dealing with a hash-based dependencies, and if the dependency is
      /// not a .swiftmodule, we can avoid opening the buffer.
      std::unique_ptr<llvm::MemoryBuffer> DepBuf = nullptr;
      auto getDepBuf = [&]() -> llvm::MemoryBuffer * {
        if (DepBuf) return DepBuf.get();
        if (auto Buf = getBufferOfDependency(fs, DepName, interfacePath,
                                             diags, diagnosticLoc)) {
          DepBuf = std::move(Buf);
          return DepBuf.get();
        }
        return nullptr;
      };

      // If Dep is itself a cached .swiftmodule, pull out its deps and include
      // them in our own, so we have a single-file view of transitive deps:
      // removes redundancies, makes the cache more relocatable, and avoids
      // opening and reading multiple swiftmodules during future loads.
      if (isCachedModule(DepName)) {
        auto buf = getDepBuf();
        if (!buf) return true;
        SmallVector<FileDependency, 16> SubDeps;
        auto VI = serialization::validateSerializedAST(buf->getBuffer(),
          /*ExtendedValidationInfo=*/nullptr, &SubDeps);
        if (VI.status != serialization::Status::Valid) {
          diags.diagnose(diagnosticLoc,
                         diag::error_extracting_dependencies_from_cached_module,
                         DepName);
          return true;
        }
        for (auto const &SubDep : SubDeps) {
          if (AllDepNames.insert(SubDep.getPath()).second) {
            Deps.push_back(SubDep);
            if (dependencyTracker)
              dependencyTracker->addDependency(
                  SubDep.getPath(), /*IsSystem=*/SubDep.isSDKRelative());
          }
        }
        continue;
      }

      // Otherwise, include this dependency directly
      auto Status = getStatusOfDependency(fs, DepName, interfacePath,
                                          diags, diagnosticLoc);
      if (!Status)
        return true;

      if (IsHashBased) {
        auto buf = getDepBuf();
        if (!buf) return true;
        uint64_t hash = xxHash64(buf->getBuffer());
        Deps.push_back(
          FileDependency::hashBased(DepNameToStore, IsSDKRelative,
                                    Status->getSize(), hash));
      } else {
        uint64_t mtime =
          Status->getLastModificationTime().time_since_epoch().count();
        Deps.push_back(
          FileDependency::modTimeBased(DepNameToStore, IsSDKRelative,
                                       Status->getSize(), mtime));
      }
    }
    return false;
  }

public:
  ParseableInterfaceBuilder(ASTContext &ctx,
                            StringRef interfacePath,
                            StringRef moduleName,
                            StringRef moduleCachePath,
                            StringRef prebuiltCachePath,
                            bool serializeDependencyHashes = false,
                            bool trackSystemDependencies = false,
                            SourceLoc diagnosticLoc = SourceLoc(),
                            DependencyTracker *tracker = nullptr)
  : ctx(ctx), fs(*ctx.SourceMgr.getFileSystem()), diags(ctx.Diags),
  interfacePath(interfacePath), moduleName(moduleName),
  moduleCachePath(moduleCachePath), prebuiltCachePath(prebuiltCachePath),
  serializeDependencyHashes(serializeDependencyHashes),
  trackSystemDependencies(trackSystemDependencies),
  diagnosticLoc(diagnosticLoc), dependencyTracker(tracker) {
    configureSubInvocation();
  }

  const CompilerInvocation &getSubInvocation() const {
    return subInvocation;
  }

  bool buildSwiftModule(StringRef OutPath, bool ShouldSerializeDeps,
                        std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer) {
    bool SubError = false;
    bool RunSuccess = llvm::CrashRecoveryContext().RunSafelyOnThread([&] {
      // Note that we don't assume cachePath is the same as the Clang
      // module cache path at this point.
      if (!moduleCachePath.empty())
        (void)llvm::sys::fs::create_directory(moduleCachePath);

      configureSubInvocationInputsAndOutputs(OutPath);

      FrontendOptions &FEOpts = subInvocation.getFrontendOptions();
      const auto &InputInfo = FEOpts.InputsAndOutputs.firstInput();
      StringRef InPath = InputInfo.file();
      const auto &OutputInfo =
        InputInfo.getPrimarySpecificPaths().SupplementaryOutputs;
      StringRef OutPath = OutputInfo.ModuleOutputPath;

      llvm::BumpPtrAllocator SubArgsAlloc;
      llvm::StringSaver SubArgSaver(SubArgsAlloc);
      SmallVector<const char *, 16> SubArgs;
      swift::version::Version Vers;
      if (extractSwiftInterfaceVersionAndArgs(Vers, SubArgSaver, SubArgs)) {
        SubError = true;
        return;
      }

      // For now: we support anything with the same "major version" and assume
      // minor versions might be interesting for debugging, or special-casing a
      // compatible field variant.
      if (Vers.asMajorVersion() != InterfaceFormatVersion.asMajorVersion()) {
        diags.diagnose(diagnosticLoc,
                       diag::unsupported_version_of_parseable_interface,
                       interfacePath, Vers);
        SubError = true;
        return;
      }

      SmallString<32> ExpectedModuleName = subInvocation.getModuleName();
      if (subInvocation.parseArgs(SubArgs, diags)) {
        SubError = true;
        return;
      }

      if (subInvocation.getModuleName() != ExpectedModuleName) {
        auto DiagKind = diag::serialization_name_mismatch;
        if (subInvocation.getLangOptions().DebuggerSupport)
          DiagKind = diag::serialization_name_mismatch_repl;
        diags.diagnose(diagnosticLoc, DiagKind, subInvocation.getModuleName(),
                       ExpectedModuleName);
        SubError = true;
        return;
      }

      // Optimize emitted modules. This has to happen after we parse arguments,
      // because parseSILOpts would override the current optimization mode.
      subInvocation.getSILOptions().OptMode = OptimizationMode::ForSpeed;

      // Build the .swiftmodule; this is a _very_ abridged version of the logic
      // in performCompile in libFrontendTool, specialized, to just the one
      // module-serialization task we're trying to do here.
      LLVM_DEBUG(llvm::dbgs() << "Setting up instance to compile "
                 << InPath << " to " << OutPath << "\n");
      CompilerInstance SubInstance;
      SubInstance.getSourceMgr().setFileSystem(&fs);

      ForwardingDiagnosticConsumer FDC(diags);
      SubInstance.addDiagnosticConsumer(&FDC);

      SubInstance.createDependencyTracker(FEOpts.TrackSystemDeps);

      if (SubInstance.setup(subInvocation)) {
        SubError = true;
        return;
      }

      LLVM_DEBUG(llvm::dbgs() << "Performing sema\n");
      SubInstance.performSema();
      if (SubInstance.getASTContext().hadError()) {
        LLVM_DEBUG(llvm::dbgs() << "encountered errors\n");
        SubError = true;
        return;
      }

      SILOptions &SILOpts = subInvocation.getSILOptions();
      auto Mod = SubInstance.getMainModule();
      auto SILMod = performSILGeneration(Mod, SILOpts);
      if (!SILMod) {
        LLVM_DEBUG(llvm::dbgs() << "SILGen did not produce a module\n");
        SubError = true;
        return;
      }

      // Setup the callbacks for serialization, which can occur during the
      // optimization pipeline.
      SerializationOptions SerializationOpts;
      std::string OutPathStr = OutPath;
      SerializationOpts.OutputPath = OutPathStr.c_str();
      SerializationOpts.ModuleLinkName = FEOpts.ModuleLinkName;
      SmallVector<FileDependency, 16> Deps;
      if (collectDepsForSerialization(SubInstance, Deps,
            FEOpts.SerializeParseableModuleInterfaceDependencyHashes)) {
        SubError = true;
        return;
      }
      if (ShouldSerializeDeps)
        SerializationOpts.Dependencies = Deps;
      SILMod->setSerializeSILAction([&]() {
        // We don't want to serialize module docs in the cache -- they
        // will be serialized beside the interface file.
        serializeToBuffers(Mod, SerializationOpts, ModuleBuffer,
                           /*ModuleDocBuffer*/nullptr, SILMod.get());
      });

      LLVM_DEBUG(llvm::dbgs() << "Running SIL processing passes\n");
      if (SubInstance.performSILProcessing(SILMod.get())) {
        LLVM_DEBUG(llvm::dbgs() << "encountered errors\n");
        SubError = true;
        return;
      }

      SubError = SubInstance.getDiags().hadAnyError();
    });
    return !RunSuccess || SubError;
  }
};

#pragma mark - Module Loading

namespace {

/// Handles the details of loading parseable interfaces as modules, and will
/// do the necessary lookup to determine if we should be loading from the
/// normal cache, the prebuilt cache, a module adjacent to the interface, or
/// a module that we'll build from a parseable interface.
class ParseableInterfaceModuleLoaderImpl {
  using AccessPathElem = std::pair<Identifier, SourceLoc>;
  friend class swift::ParseableInterfaceModuleLoader;
  ASTContext &ctx;
  llvm::vfs::FileSystem &fs;
  DiagnosticEngine &diags;
  const StringRef modulePath;
  const std::string interfacePath;
  const StringRef moduleName;
  const StringRef prebuiltCacheDir;
  const StringRef cacheDir;
  const SourceLoc diagnosticLoc;
  DependencyTracker *const dependencyTracker;
  const ModuleLoadingMode loadMode;

  ParseableInterfaceModuleLoaderImpl(
    ASTContext &ctx, StringRef modulePath, StringRef interfacePath,
    StringRef moduleName, StringRef cacheDir, StringRef prebuiltCacheDir,
    SourceLoc diagLoc, DependencyTracker *dependencyTracker = nullptr,
    ModuleLoadingMode loadMode = ModuleLoadingMode::PreferSerialized)
  : ctx(ctx), fs(*ctx.SourceMgr.getFileSystem()), diags(ctx.Diags),
    modulePath(modulePath), interfacePath(interfacePath),
    moduleName(moduleName), prebuiltCacheDir(prebuiltCacheDir),
    cacheDir(cacheDir), diagnosticLoc(diagLoc),
    dependencyTracker(dependencyTracker), loadMode(loadMode) {}

  /// Construct a cache key for the .swiftmodule being generated. There is a
  /// balance to be struck here between things that go in the cache key and
  /// things that go in the "up to date" check of the cache entry. We want to
  /// avoid fighting over a single cache entry too much when (say) running
  /// different compiler versions on the same machine or different inputs
  /// that happen to have the same short module name, so we will disambiguate
  /// those in the key. But we want to invalidate and rebuild a cache entry
  /// -- rather than making a new one and potentially filling up the cache
  /// with dead entries -- when other factors change, such as the contents of
  /// the .swiftinterface input or its dependencies.
  std::string getCacheHash(const CompilerInvocation &SubInvocation) {
    // Start with the compiler version (which will be either tag names or revs).
    // Explicitly don't pass in the "effective" language version -- this would
    // mean modules built in different -swift-version modes would rebuild their
    // dependencies.
    llvm::hash_code H = hash_value(swift::version::getSwiftFullVersion());

    // Simplest representation of input "identity" (not content) is just a
    // pathname, and probably all we can get from the VFS in this regard
    // anyways.
    H = hash_combine(H, interfacePath);

    // Include the target CPU architecture. In practice, .swiftinterface files
    // will be in architecture-specific subdirectories and would have
    // architecture-specific pieces #if'd out. However, it doesn't hurt to
    // include it, and it guards against mistakenly reusing cached modules
    // across architectures.
    H = hash_combine(H, SubInvocation.getLangOptions().Target.getArchName());

    // The SDK path is going to affect how this module is imported, so include
    // it.
    H = hash_combine(H, SubInvocation.getSDKPath());

    // Whether or not we're tracking system dependencies affects the
    // invalidation behavior of this cache item.
    H = hash_combine(H, SubInvocation.getFrontendOptions().TrackSystemDeps);

    return llvm::APInt(64, H).toString(36, /*Signed=*/false);
  }

  /// Calculate an output filename in \p SubInvocation's cache path that
  /// includes a hash of relevant key data.
  void computeCachedOutputPath(const CompilerInvocation &SubInvocation,
                               llvm::SmallString<256> &OutPath) {
    OutPath = SubInvocation.getClangModuleCachePath();
    llvm::sys::path::append(OutPath, SubInvocation.getModuleName());
    OutPath.append("-");
    OutPath.append(getCacheHash(SubInvocation));
    OutPath.append(".");
    auto OutExt = file_types::getExtension(file_types::TY_SwiftModuleFile);
    OutPath.append(OutExt);
  }

  // Checks that a dependency read from the cached module is up to date compared
  // to the interface file it represents.
  bool dependencyIsUpToDate(const FileDependency &dep, StringRef fullPath) {
    auto status = getStatusOfDependency(fs, fullPath, interfacePath,
                                        diags, diagnosticLoc);
    if (!status) return false;

    // If the sizes differ, then we know the file has changed.
    if (status->getSize() != dep.getSize()) return false;

    // Otherwise, if this dependency is verified by modification time, check
    // it vs. the modification time of the file.
    if (dep.isModificationTimeBased()) {
      uint64_t mtime =
        status->getLastModificationTime().time_since_epoch().count();
      return mtime == dep.getModificationTime();
    }

    // Slow path: if the dependency is verified by content hash, check it vs.
    // the hash of the file.
    auto buf = getBufferOfDependency(fs, fullPath, interfacePath,
                                     diags, diagnosticLoc);
    if (!buf) return false;

    return xxHash64(buf->getBuffer()) == dep.getContentHash();
  }

  // Check if all the provided file dependencies are up-to-date compared to
  // what's currently on disk.
  bool dependenciesAreUpToDate(ArrayRef<FileDependency> deps) {
    SmallString<128> SDKRelativeBuffer;
    for (auto &in : deps) {
      StringRef fullPath = in.getPath();
      if (in.isSDKRelative()) {
        SDKRelativeBuffer = ctx.SearchPathOpts.SDKPath;
        llvm::sys::path::append(SDKRelativeBuffer, in.getPath());
        fullPath = SDKRelativeBuffer.str();
      }
      if (dependencyTracker)
        dependencyTracker->addDependency(fullPath, /*isSystem*/in.isSDKRelative());
      if (!dependencyIsUpToDate(in, fullPath)) {
        LLVM_DEBUG(llvm::dbgs() << "Dep " << in.getPath()
                   << " is directly out of date\n");
        return false;
      }
      LLVM_DEBUG(llvm::dbgs() << "Dep " << in.getPath() << " is up to date\n");
    }
    return true;
  }

  // Check that the output .swiftmodule file is at least as new as all the
  // dependencies it read when it was built last time.
  bool serializedASTBufferIsUpToDate(
    const llvm::MemoryBuffer &buf, SmallVectorImpl<FileDependency> &allDeps) {
    LLVM_DEBUG(llvm::dbgs() << "Validating deps of " << modulePath << "\n");
    auto validationInfo = serialization::validateSerializedAST(
        buf.getBuffer(), /*ExtendedValidationInfo=*/nullptr, &allDeps);

    if (validationInfo.status != serialization::Status::Valid)
      return false;

    return dependenciesAreUpToDate(allDeps);
  }

  // Check that the output .swiftmodule file is at least as new as all the
  // dependencies it read when it was built last time.
  bool swiftModuleIsUpToDate(
    StringRef modulePath, SmallVectorImpl<FileDependency> &AllDeps,
    std::unique_ptr<llvm::MemoryBuffer> &moduleBuffer) {
    auto OutBuf = fs.getBufferForFile(modulePath);
    if (!OutBuf)
      return false;
    moduleBuffer = std::move(*OutBuf);
    return serializedASTBufferIsUpToDate(*moduleBuffer, AllDeps);
  }

  // Check that a "forwarding" .swiftmodule file is at least as new as all the
  // dependencies it read when it was built last time. Requires that the
  // forwarding module has been loaded from disk.
  bool forwardingModuleIsUpToDate(
    const ForwardingModule &fwd, SmallVectorImpl<FileDependency> &deps,
    std::unique_ptr<llvm::MemoryBuffer> &moduleBuffer) {
    // First, make sure the underlying module path exists and is valid.
    auto modBuf = fs.getBufferForFile(fwd.underlyingModulePath);
    if (!modBuf || !serializedASTLooksValid(*modBuf.get()))
      return false;

    // Next, check the dependencies in the forwarding file.
    for (auto &dep : fwd.dependencies) {
      // Forwarding modules expand SDKRelative paths when generated, so are
      // guaranteed to be absolute.
      deps.push_back(
        FileDependency::modTimeBased(
          dep.path, /*isSDKRelative=*/false, dep.size,
          dep.lastModificationTime));
    }
    if (!dependenciesAreUpToDate(deps))
      return false;

    moduleBuffer = std::move(*modBuf);
    return true;
  }

  Optional<StringRef>
  computePrebuiltModulePath(llvm::SmallString<256> &scratch) {
    namespace path = llvm::sys::path;
    StringRef sdkPath = ctx.SearchPathOpts.SDKPath;

    // Check if the interface file comes from the SDK
    if (sdkPath.empty() || !hasPrefix(path::begin(interfacePath),
                                      path::end(interfacePath),
                                      path::begin(sdkPath),
                                      path::end(sdkPath)))
      return None;

    // Assemble the expected path: $PREBUILT_CACHE/Foo.swiftmodule or
    // $PREBUILT_CACHE/Foo.swiftmodule/arch.swiftmodule. Note that there's no
    // cache key here.
    scratch.append(prebuiltCacheDir);

    // FIXME: Would it be possible to only have architecture-specific names
    // here? Then we could skip this check.
    StringRef inParentDirName =
      path::filename(path::parent_path(interfacePath));
    if (path::extension(inParentDirName) == ".swiftmodule") {
      assert(path::stem(inParentDirName) == moduleName);
      path::append(scratch, inParentDirName);
    }
    path::append(scratch, path::filename(modulePath));

    return scratch.str();
  }

  /// Finds the most appropriate .swiftmodule, whose dependencies are up to
  /// date, that we can load for the provided .swiftinterface file.
  llvm::ErrorOr<DiscoveredModule> discoverUpToDateModuleForInterface(
    StringRef modulePath, StringRef cachedOutputPath,
    SmallVectorImpl<FileDependency> &deps) {
    auto notFoundError =
      std::make_error_code(std::errc::no_such_file_or_directory);

    // Keep track of whether we should attempt to load a .swiftmodule adjacent
    // to the .swiftinterface.
    bool shouldLoadAdjacentModule = true;

    switch (loadMode) {
    case ModuleLoadingMode::OnlyParseable:
      // Always skip both the caches and adjacent modules, and always build the
      // parseable interface.
      return notFoundError;
    case ModuleLoadingMode::PreferParseable:
      // If we're in the load mode that prefers .swiftinterfaces, specifically
      // skip the module adjacent to the interface, but use the caches if
      // they're present.
      shouldLoadAdjacentModule = false;
      break;
    case ModuleLoadingMode::PreferSerialized:
      // The rest of the function should be covered by this.
      break;
    case ModuleLoadingMode::OnlySerialized:
      llvm_unreachable("parseable module loader should not have been created");
    }

    // First, check the cached module path. Whatever's in this cache represents
    // the most up-to-date knowledge we have about the module.
    if (auto cachedBufOrError = fs.getBufferForFile(cachedOutputPath)) {
      auto buf = std::move(*cachedBufOrError);

      // Check to see if the module is a serialized AST. If it's not, then we're
      // probably dealing with a Forwarding Module, which is a YAML file.
      bool isForwardingModule =
        !serialization::isSerializedAST(buf->getBuffer());

      // If it's a forwarding module, load the YAML file from disk and check
      // if it's up-to-date.
      if (isForwardingModule) {
        if (auto forwardingModule = ForwardingModule::load(*buf)) {
          std::unique_ptr<llvm::MemoryBuffer> moduleBuffer;
          if (forwardingModuleIsUpToDate(*forwardingModule, deps, moduleBuffer))
            return DiscoveredModule::forwarded(
              forwardingModule->underlyingModulePath, std::move(moduleBuffer));
        }
      // Otherwise, check if the AST buffer itself is up to date.
      } else if (serializedASTBufferIsUpToDate(*buf, deps)) {
        return DiscoveredModule::normal(cachedOutputPath, std::move(buf));
      }
    }

    // If we weren't able to open the file for any reason, including it not
    // existing, keep going.

    // If we have a prebuilt cache path, check that too if the interface comes
    // from the SDK.
    if (!prebuiltCacheDir.empty()) {
      llvm::SmallString<256> scratch;
      std::unique_ptr<llvm::MemoryBuffer> moduleBuffer;
      auto path = computePrebuiltModulePath(scratch);
      if (path && swiftModuleIsUpToDate(*path, deps, moduleBuffer))
        return DiscoveredModule::prebuilt(*path, std::move(moduleBuffer));
    }

    // Finally, if there's a module adjacent to the .swiftinterface that we can
    // _likely_ load (it validates OK and is up to date), bail early with
    // errc::not_supported, so the next (serialized) loader in the chain will
    // load it. Alternately, if there's a .swiftmodule present but we can't even
    // read it (for whatever reason), we should let the other module loader
    // diagnose it.
    if (!shouldLoadAdjacentModule)
      return notFoundError;

    auto adjacentModuleBuffer = fs.getBufferForFile(modulePath);
    if (adjacentModuleBuffer) {
      if (serializedASTBufferIsUpToDate(*adjacentModuleBuffer.get(), deps))
        return std::make_error_code(std::errc::not_supported);
    } else if (adjacentModuleBuffer.getError() != notFoundError) {
      return std::make_error_code(std::errc::not_supported);
    }

    // Couldn't find an up-to-date .swiftmodule, will need to build module from
    // interface.
    return notFoundError;
  }

  /// Writes the "forwarding module" that will forward to a module in the
  /// prebuilt cache.
  /// Since forwarding modules track dependencies separately from the module
  /// they point to, we'll need to grab the up-to-date file status while doing
  /// this.
  bool writeForwardingModule(const DiscoveredModule &mod,
                             StringRef outputPath,
                             ArrayRef<FileDependency> deps) {
    assert(mod.isPrebuilt() &&
           "cannot write forwarding file for non-prebuilt module");
    ForwardingModule fwd(mod.path);

    // FIXME: We need to avoid re-statting all these dependencies, otherwise
    //        we may record out-of-date information.
    auto addDependency = [&](StringRef path) {
      auto status = fs.status(path);
      uint64_t mtime =
        status->getLastModificationTime().time_since_epoch().count();
      fwd.addDependency(path, status->getSize(), mtime);
    };

    // Add the prebuilt module as a dependency of the forwarding module.
    addDependency(fwd.underlyingModulePath);

    // Add all the dependencies from the prebuilt module.
    SmallString<128> SDKRelativeBuffer;
    for (auto dep : deps) {
      StringRef fullPath = dep.getPath();
      if (dep.isSDKRelative()) {
        SDKRelativeBuffer = ctx.SearchPathOpts.SDKPath;
        llvm::sys::path::append(SDKRelativeBuffer, dep.getPath());
        fullPath = SDKRelativeBuffer.str();
      }
      addDependency(fullPath);
    }

    return withOutputFile(diags, outputPath,
      [&](llvm::raw_pwrite_stream &out) {
        llvm::yaml::Output yamlWriter(out);
        yamlWriter << fwd;
        return false;
      });
  }

  /// Looks up the best module to load for a given interface, and returns a
  /// buffer of the module's contents.  See the main comment in
  /// \c ParseableInterfaceModuleLoader.h for an explanation of the module
  /// loading strategy.
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
  findOrBuildLoadableModule() {

    // Track system dependencies if the parent tracker is set to do so.
    // FIXME: This means -track-system-dependencies isn't honored when the
    // top-level invocation isn't tracking dependencies
    bool trackSystemDependencies = false;
    if (dependencyTracker) {
      auto ClangDependencyTracker = dependencyTracker->getClangCollector();
      trackSystemDependencies = ClangDependencyTracker->needSystemDependencies();
    }

    // Set up a builder if we need to build the module. It'll also set up
    // the subinvocation we'll need to use to compute the cache paths.
    ParseableInterfaceBuilder builder(
      ctx, interfacePath, moduleName, cacheDir, prebuiltCacheDir,
      /*serializeDependencyHashes*/false, trackSystemDependencies,
      diagnosticLoc, dependencyTracker);
    auto &subInvocation = builder.getSubInvocation();

    // Compute the output path if we're loading or emitting a cached module.
    llvm::SmallString<256> cachedOutputPath;
    computeCachedOutputPath(subInvocation, cachedOutputPath);

    // Try to find the right module for this interface, either alongside it,
    // in the cache, or in the prebuilt cache.
    SmallVector<FileDependency, 16> allDeps;
    auto moduleOrErr =
      discoverUpToDateModuleForInterface(modulePath, cachedOutputPath, allDeps);

    // If we errored with anything other than 'no such file or directory',
    // fail this load and let the other module loader diagnose it.
    if (!moduleOrErr &&
        moduleOrErr.getError() != std::errc::no_such_file_or_directory)
      return moduleOrErr.getError();

    // We discovered a module! Return that module's buffer so we can load it.
    if (moduleOrErr) {
      auto module = std::move(moduleOrErr.get());

      // If it's prebuilt, use this time to generate a forwarding module.
      if (module.isPrebuilt())
        if (writeForwardingModule(module, cachedOutputPath, allDeps))
          return std::make_error_code(std::errc::not_supported);

      return std::move(module.moduleBuffer);
    }

    std::unique_ptr<llvm::MemoryBuffer> moduleBuffer;
    // We didn't discover a module corresponding to this interface. Build one.
    if (builder.buildSwiftModule(cachedOutputPath, /*shouldSerializeDeps*/true,
                                 &moduleBuffer))
      return std::make_error_code(std::errc::invalid_argument);

    assert(moduleBuffer &&
           "failed to write module buffer but returned success?");
    return std::move(moduleBuffer);
  }
};

} // end anonymous namespace

/// Load a .swiftmodule associated with a .swiftinterface either from a
/// cache or by converting it in a subordinate \c CompilerInstance, caching
/// the results.
std::error_code ParseableInterfaceModuleLoader::findModuleFilesInDirectory(
  AccessPathElem ModuleID, StringRef DirPath, StringRef ModuleFilename,
  StringRef ModuleDocFilename,
  std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
  std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer) {

  // If running in OnlySerialized mode, ParseableInterfaceModuleLoader
  // should not have been constructed at all.
  assert(LoadMode != ModuleLoadingMode::OnlySerialized);

  auto &fs = *Ctx.SourceMgr.getFileSystem();
  llvm::SmallString<256> ModPath, InPath;

  // First check to see if the .swiftinterface exists at all. Bail if not.
  ModPath = DirPath;
  path::append(ModPath, ModuleFilename);

  auto Ext = file_types::getExtension(file_types::TY_SwiftParseableInterfaceFile);
  InPath = ModPath;
  path::replace_extension(InPath, Ext);
  if (!fs.exists(InPath))
    return std::make_error_code(std::errc::no_such_file_or_directory);

  // Create an instance of the Impl to do the heavy lifting.
  ParseableInterfaceModuleLoaderImpl Impl(
                Ctx, ModPath, InPath, ModuleID.first.str(),
                CacheDir, PrebuiltCacheDir, ModuleID.second, dependencyTracker,
                LoadMode);

  // Ask the impl to find us a module that we can load or give us an error
  // telling us that we couldn't load it.
  auto ModuleBufferOrErr = Impl.findOrBuildLoadableModule();
  if (!ModuleBufferOrErr)
    return ModuleBufferOrErr.getError();

  if (ModuleBuffer) {
    *ModuleBuffer = std::move(*ModuleBufferOrErr);
  }

  // Delegate back to the serialized module loader to load the module doc.
  llvm::SmallString<256> DocPath{DirPath};
  path::append(DocPath, ModuleDocFilename);
  auto DocLoadErr =
    SerializedModuleLoaderBase::openModuleDocFile(ModuleID, DocPath,
                                                  ModuleDocBuffer);
  if (DocLoadErr)
    return DocLoadErr;

  return std::error_code();
}


bool ParseableInterfaceModuleLoader::buildSwiftModuleFromSwiftInterface(
  ASTContext &Ctx, StringRef CacheDir, StringRef PrebuiltCacheDir,
  StringRef ModuleName, StringRef InPath, StringRef OutPath,
  bool SerializeDependencyHashes, bool TrackSystemDependencies) {
  ParseableInterfaceBuilder builder(Ctx, InPath, ModuleName,
                                    CacheDir, PrebuiltCacheDir,
                                    SerializeDependencyHashes,
                                    TrackSystemDependencies);
  // FIXME: We really only want to serialize 'important' dependencies here, if
  //        we want to ship the built swiftmodules to another machine.
  return builder.buildSwiftModule(OutPath, /*shouldSerializeDeps*/true,
                                  /*ModuleBuffer*/nullptr);
}
