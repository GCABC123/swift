//===--- CursorInfo.cpp ---------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IDE/CursorInfo.h"
#include "ExprContextAnalysis.h"
#include "swift/AST/ASTDemangler.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/USRGeneration.h"
#include "swift/IDE/SelectedOverloadInfo.h"
#include "swift/IDE/TypeCheckCompletionCallback.h"
#include "swift/Parse/IDEInspectionCallbacks.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/IDETypeChecking.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/Module.h"

using namespace swift;
using namespace swift::constraints;
using namespace ide;

namespace {

// MARK: - Utilities

void typeCheckDeclAndParentClosures(ValueDecl *VD) {
  if (!VD) {
    return;
  }
  // We need to type check any parent closures because their types are
  // encoded in the USR of ParentContexts in the cursor info response.
  auto DC = VD->getDeclContext();
  while (DC->getParent()) {
    if (auto Closure = dyn_cast<AbstractClosureExpr>(DC)) {
      if (Closure->getType().isNull()) {
        typeCheckASTNodeAtLoc(
            TypeCheckASTNodeAtLocContext::declContext(DC->getParent()),
            Closure->getLoc());
      }
    }
    DC = DC->getParent();
  }

  if (!VD->getInterfaceType()) {
    // The decl has an interface time if it came from another module. In that
    // case, there's nothing to do. Otherwise, type check the decl to get its
    // type.
    typeCheckASTNodeAtLoc(
        TypeCheckASTNodeAtLocContext::declContext(VD->getDeclContext()),
        VD->getLoc());
  }
  if (auto VarD = dyn_cast<VarDecl>(VD)) {
    if (VarD->hasAttachedPropertyWrapper()) {
      // Type check any attached property wrappers so the annotated declaration
      // can refer to their USRs.
      (void)VarD->getPropertyWrapperBackingPropertyType();
    }
    // Visit emitted accessors so we generated accessors from property wrappers.
    VarD->visitEmittedAccessors([&](AccessorDecl *accessor) {});
  }
}

// MARK: - NodeFinderResults

enum class NodeFinderResultKind { Decl, Expr };

class NodeFinderResult {
  NodeFinderResultKind Kind;

protected:
  NodeFinderResult(NodeFinderResultKind Kind) : Kind(Kind) {}

public:
  NodeFinderResultKind getKind() const { return Kind; }
};

class NodeFinderDeclResult : public NodeFinderResult {
  ValueDecl *ValueD;

public:
  NodeFinderDeclResult(ValueDecl *ValueD)
      : NodeFinderResult(NodeFinderResultKind::Decl), ValueD(ValueD) {}

  ValueDecl *getDecl() const { return ValueD; }

  static bool classof(const NodeFinderResult *Res) {
    return Res->getKind() == NodeFinderResultKind::Decl;
  }
};

class NodeFinderExprResult : public NodeFinderResult {
  Expr *E;
  /// The \c DeclContext in which \c E occurs.
  DeclContext *DC;

public:
  NodeFinderExprResult(Expr *E, DeclContext *DC)
      : NodeFinderResult(NodeFinderResultKind::Expr), E(E), DC(DC) {}

  Expr *getExpr() const { return E; }

  DeclContext *getDeclContext() const { return DC; }

  static bool classof(const NodeFinderResult *Res) {
    return Res->getKind() == NodeFinderResultKind::Expr;
  }
};

// MARK: - NodeFinder

/// Walks the AST, looking for a node at \c LocToResolve. While walking the
/// AST, also gathers information about shorthand shadows.
class NodeFinder : ASTWalker {
  SourceFile &SrcFile;
  SourceLoc LocToResolve;

  /// As we are walking the tree, this variable is updated to the last seen
  /// DeclContext.
  SmallVector<DeclContext *> DeclContextStack;

  /// The found node.
  std::unique_ptr<NodeFinderResult> Result;

  /// If a decl shadows another decl using shorthand syntax (`[foo]` or
  /// `if let foo {`), this maps the re-declared variable to the one that is
  /// being shadowed.
  /// The transitive closure of shorthand shadowed decls should be reported as
  /// additional results in cursor info.
  llvm::DenseMap<ValueDecl *, ValueDecl *> ShorthandShadowedDecls;

public:
  NodeFinder(SourceFile &SrcFile, SourceLoc LocToResolve)
      : SrcFile(SrcFile), LocToResolve(LocToResolve),
        DeclContextStack({&SrcFile}) {}

  void resolve() { SrcFile.walk(*this); }

  std::unique_ptr<NodeFinderResult> takeResult() { return std::move(Result); }

  /// Get the declarations that \p ShadowingDecl shadows using shorthand shadow
  /// syntax. Ordered from innermost to outermost shadows.
  SmallVector<ValueDecl *, 2>
  getShorthandShadowedDecls(ValueDecl *ShadowingDecl) {
    SmallVector<ValueDecl *, 2> Result;
    auto ShorthandShadowedDecl = ShorthandShadowedDecls[ShadowingDecl];
    while (ShorthandShadowedDecl) {
      Result.push_back(ShorthandShadowedDecl);
      ShorthandShadowedDecl = ShorthandShadowedDecls[ShorthandShadowedDecl];
    }
    return Result;
  }

private:
  SourceManager &getSourceMgr() const {
    return SrcFile.getASTContext().SourceMgr;
  }

  /// The decl context that is currently being walked.
  DeclContext *getCurrentDeclContext() { return DeclContextStack.back(); }

  bool rangeContainsLocToResolve(SourceRange Range) const {
    return getSourceMgr().containsRespectingReplacedRanges(Range, LocToResolve);
  }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    if (!rangeContainsLocToResolve(D->getSourceRangeIncludingAttrs())) {
      return PreWalkAction::SkipChildren;
    }

    if (auto *newDC = dyn_cast<DeclContext>(D)) {
      DeclContextStack.push_back(newDC);
    }

    if (D->getLoc() != LocToResolve) {
      return Action::Continue();
    }

    if (auto VD = dyn_cast<ValueDecl>(D)) {
      // FIXME: ParamDecls might be closure parameters that can have ambiguous
      // types. The current infrastructure of just asking for the VD's type
      // doesn't work here. We need to inspect the constraints system solution.
      if (VD->hasName() && !isa<ParamDecl>(D)) {
        assert(Result == nullptr);
        Result = std::make_unique<NodeFinderDeclResult>(VD);
        return Action::Stop();
      }
    }

    return Action::Continue();
  }

  PostWalkAction walkToDeclPost(Decl *D) override {
    if (auto *newDC = dyn_cast<DeclContext>(D)) {
      assert(DeclContextStack.back() == newDC);
      DeclContextStack.pop_back();
    }
    return Action::Continue();
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (auto closure = dyn_cast<ClosureExpr>(E)) {
      DeclContextStack.push_back(closure);
    }

    if (auto CaptureList = dyn_cast<CaptureListExpr>(E)) {
      for (auto ShorthandShadows :
           getShorthandShadows(CaptureList, getCurrentDeclContext())) {
        assert(ShorthandShadowedDecls.count(ShorthandShadows.first) == 0);
        ShorthandShadowedDecls[ShorthandShadows.first] =
            ShorthandShadows.second;
      }
    }

    if (E->getLoc() != LocToResolve) {
      return Action::Continue(E);
    }

    switch (E->getKind()) {
    case ExprKind::DeclRef:
    case ExprKind::UnresolvedDot:
    case ExprKind::UnresolvedDeclRef: {
      assert(Result == nullptr);
      Result =
          std::make_unique<NodeFinderExprResult>(E, getCurrentDeclContext());
      return Action::Stop();
    }
    default:
      break;
    }

    return Action::Continue(E);
  }

  PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
    if (auto *closure = dyn_cast<ClosureExpr>(E)) {
      assert(DeclContextStack.back() == closure);
      DeclContextStack.pop_back();
    }
    return Action::Continue(E);
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    if (auto CondStmt = dyn_cast<LabeledConditionalStmt>(S)) {
      for (auto ShorthandShadow :
           getShorthandShadows(CondStmt, getCurrentDeclContext())) {
        assert(ShorthandShadowedDecls.count(ShorthandShadow.first) == 0);
        ShorthandShadowedDecls[ShorthandShadow.first] = ShorthandShadow.second;
      }
    }
    return Action::Continue(S);
  }
};

// MARK: - Solver-based expression analysis

class CursorInfoTypeCheckSolutionCallback : public TypeCheckCompletionCallback {
public:
  struct CursorInfoDeclReference {
    /// If the referenced declaration is a member reference, the type of the
    /// member's base, otherwise \c null.
    Type BaseType;
    /// Whether the reference is dynamic (see \c ide::isDynamicRef)
    bool IsDynamicRef;
    /// The declaration that is being referenced. Will never be \c nullptr.
    ValueDecl *ReferencedDecl;
  };

private:
  /// The expression for which we want to provide cursor info results.
  Expr *ResolveExpr;

  SmallVector<CursorInfoDeclReference, 1> Results;

  void sawSolutionImpl(const Solution &S) override {
    auto &CS = S.getConstraintSystem();

    auto Locator = CS.getConstraintLocator(ResolveExpr);
    auto CalleeLocator = S.getCalleeLocator(Locator);
    auto OverloadInfo = getSelectedOverloadInfo(S, CalleeLocator);
    if (!OverloadInfo.Value) {
      // We could not resolve the referenced declaration. Skip the solution.
      return;
    }

    bool IsDynamicRef = false;
    auto BaseLocator =
        CS.getConstraintLocator(Locator, ConstraintLocator::MemberRefBase);
    if (auto BaseExpr =
            simplifyLocatorToAnchor(BaseLocator).dyn_cast<Expr *>()) {
      IsDynamicRef =
          ide::isDynamicRef(BaseExpr, OverloadInfo.Value,
                            [&S](Expr *E) { return S.getResolvedType(E); });
    }

    Results.push_back({OverloadInfo.BaseTy, IsDynamicRef, OverloadInfo.Value});
  }

public:
  CursorInfoTypeCheckSolutionCallback(Expr *ResolveExpr)
      : ResolveExpr(ResolveExpr) {}

  ArrayRef<CursorInfoDeclReference> getResults() const { return Results; }
};

// MARK: - CursorInfoDoneParsingCallback

class CursorInfoDoneParsingCallback : public IDEInspectionCallbacks {
  CursorInfoConsumer &Consumer;
  SourceLoc RequestedLoc;

public:
  CursorInfoDoneParsingCallback(Parser &P, CursorInfoConsumer &Consumer,
                                SourceLoc RequestedLoc)
      : IDEInspectionCallbacks(P), Consumer(Consumer),
        RequestedLoc(RequestedLoc) {}

  ResolvedCursorInfoPtr getDeclResult(NodeFinderDeclResult *DeclResult,
                                      SourceFile *SrcFile,
                                      NodeFinder &Finder) const {
    typeCheckDeclAndParentClosures(DeclResult->getDecl());
    return new ResolvedValueRefCursorInfo(
        SrcFile, RequestedLoc, DeclResult->getDecl(),
        /*CtorTyRef=*/nullptr,
        /*ExtTyRef=*/nullptr, /*IsRef=*/false, /*Ty=*/Type(),
        /*ContainerType=*/Type(),
        /*CustomAttrRef=*/None,
        /*IsKeywordArgument=*/false,
        /*IsDynamic=*/false,
        /*ReceiverTypes=*/{},
        Finder.getShorthandShadowedDecls(DeclResult->getDecl()));
  }

  ResolvedCursorInfoPtr getExprResult(NodeFinderExprResult *ExprResult,
                                      SourceFile *SrcFile,
                                      NodeFinder &Finder) const {
    Expr *E = ExprResult->getExpr();
    DeclContext *DC = ExprResult->getDeclContext();

    // Type check the statemnt containing E and listen for solutions.
    CursorInfoTypeCheckSolutionCallback Callback(E);
    llvm::SaveAndRestore<TypeCheckCompletionCallback *> CompletionCollector(
        DC->getASTContext().SolutionCallback, &Callback);
    typeCheckASTNodeAtLoc(TypeCheckASTNodeAtLocContext::declContext(DC),
                          E->getLoc());

    if (Callback.getResults().empty()) {
      // No results.
      return nullptr;
    }

    for (auto Info : Callback.getResults()) {
      // Type check the referenced decls so that all their parent closures are
      // type-checked (see comment in typeCheckDeclAndParentClosures).
      typeCheckDeclAndParentClosures(Info.ReferencedDecl);
    }

    if (Callback.getResults().size() != 1) {
      // FIXME: We need to be able to report multiple results.
      return nullptr;
    }

    // Deliver results

    auto Res = Callback.getResults()[0];
    SmallVector<NominalTypeDecl *> ReceiverTypes;
    if (Res.IsDynamicRef && Res.BaseType) {
      if (auto ReceiverType = Res.BaseType->getAnyNominal()) {
        ReceiverTypes = {ReceiverType};
      } else if (auto MT = Res.BaseType->getAs<AnyMetatypeType>()) {
        // Look through metatypes to get the nominal type decl.
        if (auto ReceiverType = MT->getInstanceType()->getAnyNominal()) {
          ReceiverTypes = {ReceiverType};
        }
      }
    }

    return new ResolvedValueRefCursorInfo(
        SrcFile, RequestedLoc, Res.ReferencedDecl,
        /*CtorTyRef=*/nullptr,
        /*ExtTyRef=*/nullptr, /*IsRef=*/true, /*Ty=*/Type(),
        /*ContainerType=*/Res.BaseType,
        /*CustomAttrRef=*/None,
        /*IsKeywordArgument=*/false, Res.IsDynamicRef, ReceiverTypes,
        Finder.getShorthandShadowedDecls(Res.ReferencedDecl));
  }

  void doneParsing(SourceFile *SrcFile) override {
    if (!SrcFile) {
      return;
    }
    NodeFinder Finder(*SrcFile, RequestedLoc);
    Finder.resolve();
    auto Result = Finder.takeResult();
    if (!Result) {
      return;
    }
    ResolvedCursorInfoPtr CursorInfo;
    switch (Result->getKind()) {
    case NodeFinderResultKind::Decl:
      CursorInfo = getDeclResult(cast<NodeFinderDeclResult>(Result.get()),
                                 SrcFile, Finder);
      break;
    case NodeFinderResultKind::Expr:
      CursorInfo = getExprResult(cast<NodeFinderExprResult>(Result.get()),
                                 SrcFile, Finder);
      break;
    }
    if (Result) {
      Consumer.handleResults(CursorInfo);
    }
  }
};

} // anonymous namespace.

IDEInspectionCallbacksFactory *
swift::ide::makeCursorInfoCallbacksFactory(CursorInfoConsumer &Consumer,
                                           SourceLoc RequestedLoc) {
  class CursorInfoCallbacksFactoryImpl : public IDEInspectionCallbacksFactory {
    CursorInfoConsumer &Consumer;
    SourceLoc RequestedLoc;

  public:
    CursorInfoCallbacksFactoryImpl(CursorInfoConsumer &Consumer,
                                   SourceLoc RequestedLoc)
        : Consumer(Consumer), RequestedLoc(RequestedLoc) {}

    IDEInspectionCallbacks *createIDEInspectionCallbacks(Parser &P) override {
      return new CursorInfoDoneParsingCallback(P, Consumer, RequestedLoc);
    }
  };

  return new CursorInfoCallbacksFactoryImpl(Consumer, RequestedLoc);
}
