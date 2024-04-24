//===--- SILGenConcurrency.cpp - Concurrency-specific SILGen --------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "ArgumentSource.h"
#include "ExecutorBreadcrumb.h"
#include "RValue.h"
#include "Scope.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Availability.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/Basic/Range.h"

using namespace swift;
using namespace Lowering;

void SILGenFunction::emitExpectedExecutor() {
  // Whether the given declaration context is nested within an actor's
  // destructor.
  auto isInActorDestructor = [](DeclContext *dc) {
    while (!dc->isModuleScopeContext() && !dc->isTypeContext()) {
      if (auto destructor = dyn_cast<DestructorDecl>(dc)) {
        switch (getActorIsolation(destructor)) {
        case ActorIsolation::ActorInstance:
          return true;

        case ActorIsolation::GlobalActor:
          // Global-actor-isolated types should likely have deinits that
          // are not themselves actor-isolated, yet still have access to
          // the instance properties of the class.
          return false;

        case ActorIsolation::Nonisolated:
        case ActorIsolation::NonisolatedUnsafe:
        case ActorIsolation::Unspecified:
          return false;

        case ActorIsolation::Erased:
          llvm_unreachable("deinit cannot have erased isolation");
        }
      }

      dc = dc->getParent();
    }

    return false;
  };

  // Initialize ExpectedExecutor if:
  // - this function is async or
  // - this function is sync and isolated to an actor, and we want to
  //   dynamically check that we're on the right executor.
  //
  // Actor destructors are isolated in the sense that we now have a
  // unique reference to the actor, but we probably aren't running on
  // the actor's executor, so we cannot safely do this check.
  //
  // Defer bodies are always called synchronously within their enclosing
  // function, so the check is unnecessary; in addition, we cannot
  // necessarily perform the check because the defer may not have
  // captured the isolated parameter of the enclosing function.
  bool wantDataRaceChecks = getOptions().EnableActorDataRaceChecks &&
      !F.isAsync() &&
      !isInActorDestructor(FunctionDC) &&
      !F.isDefer();

  // FIXME: Avoid loading and checking the expected executor if concurrency is
  // unavailable. This is specifically relevant for MainActor isolated contexts,
  // which are allowed to be available on OSes where concurrency is not
  // available. rdar://106827064

  // Local function to load the expected executor from a local actor
  auto loadExpectedExecutorForLocalVar = [&](VarDecl *var) {
    auto loc = RegularLocation::getAutoGeneratedLocation(F.getLocation());
    Type actorType = var->getTypeInContext();
    RValue actorInstanceRV = emitRValueForDecl(
        loc, var, actorType, AccessSemantics::Ordinary);
    ManagedValue actorInstance =
        std::move(actorInstanceRV).getScalarValue();
    ExpectedExecutor = emitLoadActorExecutor(loc, actorInstance);
  };

  if (auto *funcDecl =
        dyn_cast_or_null<AbstractFunctionDecl>(FunctionDC->getAsDecl())) {
    auto actorIsolation = getActorIsolation(funcDecl);
    switch (actorIsolation.getKind()) {
    case ActorIsolation::Unspecified:
    case ActorIsolation::Nonisolated:
    case ActorIsolation::NonisolatedUnsafe:
      break;

    case ActorIsolation::Erased:
      llvm_unreachable("method cannot have erased isolation");

    case ActorIsolation::ActorInstance: {
      // Only produce an executor for actor-isolated functions that are async
      // or are local functions. The former require a hop, while the latter
      // are prone to dynamic data races in code that does not enforce Sendable
      // completely.
      if (F.isAsync() ||
          (wantDataRaceChecks && funcDecl->isLocalCapture())) {
        auto loweredCaptures = SGM.Types.getLoweredLocalCaptures(SILDeclRef(funcDecl));
        if (auto isolatedParam = loweredCaptures.getIsolatedParamCapture()) {
          loadExpectedExecutorForLocalVar(isolatedParam);
        } else {
          auto loc = RegularLocation::getAutoGeneratedLocation(F.getLocation());
          ManagedValue actorArg;
          if (actorIsolation.getActorInstanceParameter() == 0) {
            ManagedValue selfArg;
            if (F.getSelfArgument()->getOwnershipKind() ==
                OwnershipKind::Guaranteed) {
              selfArg = ManagedValue::forBorrowedRValue(F.getSelfArgument());
            } else {
              selfArg =
                  ManagedValue::forUnmanagedOwnedValue(F.getSelfArgument());
            }
            ExpectedExecutor = emitLoadActorExecutor(loc, selfArg);
          } else {
            unsigned isolatedParamIdx =
                actorIsolation.getActorInstanceParameter() - 1;
            auto param = funcDecl->getParameters()->get(isolatedParamIdx);
            assert(param->isIsolated());
            loadExpectedExecutorForLocalVar(param);
          }
        }
      }
      break;
    }

    case ActorIsolation::GlobalActor:
      if (F.isAsync() || wantDataRaceChecks) {
        ExpectedExecutor =
          emitLoadGlobalActorExecutor(actorIsolation.getGlobalActor());
      }
      break;
    }
  } else if (auto *closureExpr = dyn_cast<AbstractClosureExpr>(FunctionDC)) {
    bool wantExecutor = F.isAsync() || wantDataRaceChecks;
    auto actorIsolation = closureExpr->getActorIsolation();
    switch (actorIsolation.getKind()) {
    case ActorIsolation::Unspecified:
    case ActorIsolation::Nonisolated:
    case ActorIsolation::NonisolatedUnsafe:
      break;

    case ActorIsolation::Erased:
      llvm_unreachable("closure cannot have erased isolation");

    case ActorIsolation::ActorInstance: {
      if (wantExecutor) {
        loadExpectedExecutorForLocalVar(actorIsolation.getActorInstance());
      }
      break;
    }

    case ActorIsolation::GlobalActor:
      if (wantExecutor) {
        ExpectedExecutor =
          emitLoadGlobalActorExecutor(actorIsolation.getGlobalActor());
        break;
      }
    }
  }

  // In async functions, the generic executor is our expected executor
  // if we don't have any sort of isolation.
  if (!ExpectedExecutor && F.isAsync() && !unsafelyInheritsExecutor()) {
    ExpectedExecutor = emitGenericExecutor(
      RegularLocation::getAutoGeneratedLocation(F.getLocation()));
  }

  // Jump to the expected executor.
  if (ExpectedExecutor) {
    if (F.isAsync()) {
      // For an async function, hop to the executor.
      B.createHopToExecutor(
          RegularLocation::getDebugOnlyLocation(F.getLocation(), getModule()),
          ExpectedExecutor,
          /*mandatory*/ false);
    } else {
      // For a synchronous function, check that we're on the same executor.
      // Note: if we "know" that the code is completely Sendable-safe, this
      // is unnecessary. The type checker will need to make this determination.
      emitPreconditionCheckExpectedExecutor(
                    RegularLocation::getAutoGeneratedLocation(F.getLocation()),
                    ExpectedExecutor);
    }
  }
}

void SILGenFunction::emitConstructorPrologActorHop(
    SILLocation loc, std::optional<ActorIsolation> maybeIso) {
  loc = loc.asAutoGenerated();
  if (maybeIso) {
    auto iso = *maybeIso;
    std::optional<ManagedValue> maybeSelf;
    if (iso == ActorIsolation::ActorInstance) {
      auto actor = iso.getActorInstance();
      Type actorType = actor->getTypeInContext();
      RValue actorInstanceRV = emitRValueForDecl(loc, actor, actorType, AccessSemantics::Ordinary);
      maybeSelf = std::move(actorInstanceRV).getScalarValue();
    }
    if (auto executor = emitExecutor(loc, iso, maybeSelf)) {
      ExpectedExecutor = *executor;
    }
  }

  if (!ExpectedExecutor)
    ExpectedExecutor = emitGenericExecutor(loc);

  B.createHopToExecutor(loc, ExpectedExecutor, /*mandatory*/ false);
}

void SILGenFunction::emitPrologGlobalActorHop(SILLocation loc,
                                              Type globalActor) {
  ExpectedExecutor = emitLoadGlobalActorExecutor(globalActor);
  B.createHopToExecutor(RegularLocation::getDebugOnlyLocation(loc, getModule()),
                        ExpectedExecutor, /*mandatory*/ false);
}


SILValue SILGenFunction::emitMainExecutor(SILLocation loc) {
  auto &ctx = getASTContext();
  auto builtinName = ctx.getIdentifier(
      getBuiltinName(BuiltinValueKind::BuildMainActorExecutorRef));
  auto resultType = SILType::getPrimitiveObjectType(ctx.TheExecutorType);

  return B.createBuiltin(loc, builtinName, resultType, {}, {});
}

SILValue SILGenFunction::emitGenericExecutor(SILLocation loc) {
  // The generic executor is encoded as the nil value of
  // std::optional<Builtin.SerialExecutor>.
  auto ty = SILType::getOptionalType(
              SILType::getPrimitiveObjectType(
                getASTContext().TheExecutorType));
  return B.createOptionalNone(loc, ty);
}

ManagedValue SILGenFunction::emitNonIsolatedIsolation(SILLocation loc) {
  return B.createManagedOptionalNone(loc,
                     SILType::getOpaqueIsolationType(getASTContext()));
}

SILValue SILGenFunction::emitLoadGlobalActorExecutor(Type globalActor) {
  auto loc = RegularLocation::getAutoGeneratedLocation(F.getLocation());
  auto actorAndFormalType =
    emitLoadOfGlobalActorShared(loc, globalActor->getCanonicalType());
  return emitLoadActorExecutor(loc, actorAndFormalType.first);
}

std::pair<ManagedValue, CanType>
SILGenFunction::emitLoadOfGlobalActorShared(SILLocation loc, CanType actorType) {
  NominalTypeDecl *nominal = actorType->getNominalOrBoundGenericNominal();
  VarDecl *sharedInstanceDecl = nominal->getGlobalActorInstance();
  assert(sharedInstanceDecl && "no shared actor field in global actor");
  SubstitutionMap subs =
    actorType->getContextSubstitutionMap(SGM.SwiftModule, nominal);
  Type instanceType =
    actorType->getTypeOfMember(SGM.SwiftModule, sharedInstanceDecl);

  auto metaRepr =
    nominal->isResilient(SGM.SwiftModule, F.getResilienceExpansion())
    ? MetatypeRepresentation::Thick
    : MetatypeRepresentation::Thin;

  CanType actorMetaType = CanMetatypeType::get(actorType, metaRepr);
  ManagedValue actorMetaTypeValue =
      ManagedValue::forObjectRValueWithoutOwnership(B.createMetatype(
          loc, SILType::getPrimitiveObjectType(actorMetaType)));

  RValue actorInstanceRV = emitRValueForStorageLoad(loc, actorMetaTypeValue,
    actorMetaType, /*isSuper*/ false, sharedInstanceDecl, PreparedArguments(),
    subs, AccessSemantics::Ordinary, instanceType, SGFContext());
  ManagedValue actorInstance = std::move(actorInstanceRV).getScalarValue();
  return {actorInstance, instanceType->getCanonicalType()};
}

ManagedValue
SILGenFunction::emitGlobalActorIsolation(SILLocation loc,
                                         CanType globalActorType) {
  // Load the .shared property.  Note that this isn't necessarily a value
  // of the global actor type.
  auto actorAndFormalType = emitLoadOfGlobalActorShared(loc, globalActorType);

  // Since it's just a normal actor instance, we can use the normal path.
  return emitActorInstanceIsolation(loc, actorAndFormalType.first,
                                    actorAndFormalType.second);
}

/// Given a value of some non-optional distributed actor type, convert it
/// to the non-optional `any Actor` type.
static ManagedValue
emitDistributedActorIsolation(SILGenFunction &SGF, SILLocation loc,
                              ManagedValue actor, CanType actorType) {
  // First, open the actor type if it's an existential type.
  if (actorType->isExistentialType()) {
    CanType openedType = OpenedArchetypeType::getAny(actorType,
                                                     SGF.F.getGenericSignature());
    SILType loweredOpenedType = SGF.getLoweredType(openedType);

    actor = SGF.emitOpenExistential(loc, actor, loweredOpenedType,
                                    AccessKind::Read);
    actorType = openedType;
  }

  auto &ctx = SGF.getASTContext();
  auto distributedActorProto =
    ctx.getProtocol(KnownProtocolKind::DistributedActor);

  // Build <T: DistributedActor> and its substitutions for actorType.
  // Doing this manually is ill-advised in general, but this is such a
  // simple case that it's okay.
  auto sig = distributedActorProto->getGenericSignature();
  auto distributedActorConf =
    SGF.SGM.SwiftModule->lookupConformance(actorType, distributedActorProto);
  auto distributedActorSubs = SubstitutionMap::get(sig, {actorType},
                                                   {distributedActorConf});

  // Use that to build the magical conformance to Actor for the distributed
  // actor type.
  return SGF.emitDistributedActorAsAnyActor(loc, distributedActorSubs, actor);
}

/// Given a value of some non-optional actor type, convert it to
/// non-optional `any Actor` type.
static ManagedValue
emitNonOptionalActorInstanceIsolation(SILGenFunction &SGF, SILLocation loc,
                                      ManagedValue actor, CanType actorType,
                                      SILType anyActorTy) {
  // If we have an `any Actor` already, we're done.
  if (actor.getType() == anyActorTy)
    return actor;

  CanType anyActorType = anyActorTy.getASTType();

  // If the actor is a distributed actor, (1) it had better be local
  // and (2) we need to use the special conformance.
  if (actorType->isDistributedActor()) {
    return emitDistributedActorIsolation(SGF, loc, actor, actorType);
  }

  return SGF.emitTransformExistential(loc, actor, actorType, anyActorType);
}

ManagedValue
SILGenFunction::emitActorInstanceIsolation(SILLocation loc, ManagedValue actor,
                                           CanType actorType) {
  // $Optional<any Actor>
  auto optionalAnyActorTy = SILType::getOpaqueIsolationType(getASTContext());
  // Optional<any Actor> as a formal type (it's invariant to lowering)
  auto optionalAnyActorType = optionalAnyActorTy.getASTType();

  // If we started with an Optional<any Actor>, we're done.
  if (actorType == optionalAnyActorType) {
    return actor;
  }

  // Otherwise, if we have an optional value, we need to transform the payload.
  auto actorObjectType = actorType.getOptionalObjectType();
  if (actorObjectType) {
    return emitOptionalToOptional(loc, actor, optionalAnyActorTy,
        [&](SILGenFunction &SGF, SILLocation loc, ManagedValue actorObject,
            SILType anyActorTy, SGFContext C) {
      return emitNonOptionalActorInstanceIsolation(*this, loc, actorObject,
                                                   actorObjectType, anyActorTy);
    });
  }

  // Otherwise, transform the non-optional value we have, then inject that
  // into Optional.
  SILType anyActorTy = optionalAnyActorTy.getOptionalObjectType();
  ManagedValue anyActor =
    emitNonOptionalActorInstanceIsolation(*this, loc, actor, actorType,
                                          anyActorTy);

  // Inject into `Optional`.
  auto result = B.createOptionalSome(loc, anyActor);
  return result;
}

SILValue SILGenFunction::emitLoadActorExecutor(SILLocation loc,
                                               ManagedValue actor) {
  // FIXME: Checking for whether we're in a formal evaluation scope
  // like this doesn't seem like a good pattern.
  SILValue actorV;
  if (isInFormalEvaluationScope())
    actorV = actor.formalAccessBorrow(*this, loc).getValue();
  else
    actorV = actor.borrow(*this, loc).getValue();

  // For now, we just want to emit a hop_to_executor directly to the
  // actor; LowerHopToActor will add the emission logic necessary later.
  return actorV;
}

SILValue SILGenFunction::emitLoadErasedExecutor(SILLocation loc,
                                                ManagedValue fn) {
  // As with emitLoadActorExecutor, we just emit the actor reference
  // for now and let LowerHopToActor deal with the executor projection.
  return emitLoadErasedIsolation(loc, fn).getUnmanagedValue();
}

ManagedValue
SILGenFunction::emitLoadErasedIsolation(SILLocation loc,
                                        ManagedValue fn) {
  fn = fn.borrow(*this, loc);

  // This expects a borrowed function and returns a borrowed (any Actor)?.
  auto actor = B.createFunctionExtractIsolation(loc, fn.getValue());

  return ManagedValue::forBorrowedObjectRValue(actor);
}

ManagedValue
SILGenFunction::emitFunctionTypeIsolation(SILLocation loc,
                                          FunctionTypeIsolation isolation,
                                          ManagedValue fn) {
  switch (isolation.getKind()) {

  // Parameter-isolated functions don't have a specific actor they're isolated
  // to; they're essentially polymorphic over isolation.
  case FunctionTypeIsolation::Kind::Parameter:
    llvm_unreachable("cannot load isolation from parameter-isoaltion function "
                     "reference");

  // Emit nonisolated by simply emitting Optional.none in the result type.
  case FunctionTypeIsolation::Kind::NonIsolated:
    return emitNonIsolatedIsolation(loc);

  // Emit global actor isolation by loading .shared from the global actor,
  // erasing it into `any Actor`, and injecting that into Optional.
  case FunctionTypeIsolation::Kind::GlobalActor:
    return emitGlobalActorIsolation(loc,
             isolation.getGlobalActorType()->getCanonicalType());

  // Emit @isolated(any) isolation by loading the actor reference from the
  // function.
  case FunctionTypeIsolation::Kind::Erased: {
    Scope scope(*this, CleanupLocation(loc));
    auto value = emitLoadErasedIsolation(loc, fn).copy(*this, loc);
    return scope.popPreservingValue(value);
  }
  }

  llvm_unreachable("bad kind");
}

static ActorIsolation getClosureIsolationInfo(SILDeclRef constant) {
  if (auto closure = constant.getAbstractClosureExpr()) {
    return closure->getActorIsolation();
  }
  auto func = constant.getAbstractFunctionDecl();
  assert(func && "unexpected closure constant");
  return getActorIsolation(func);
}

static ManagedValue emitLoadOfCaptureIsolation(SILGenFunction &SGF,
                                               SILLocation loc,
                                               VarDecl *isolatedCapture,
                                               SILDeclRef constant,
                                               ArrayRef<ManagedValue> captureArgs) {
  auto &TC = SGF.SGM.Types;
  auto captureInfo = TC.getLoweredLocalCaptures(constant);

  auto isolatedVarType = isolatedCapture->getTypeInContext()->getCanonicalType();

  // Capture arguments are 1-1 with the lowered capture info.
  auto captures = captureInfo.getCaptures();
  for (auto i : indices(captures)) {
    const auto &capture = captures[i];
    if (capture.isDynamicSelfMetadata()) continue;
    auto capturedVar = capture.getDecl();
    if (capturedVar != isolatedCapture) continue;

    // Captured actor references should always be captured as constants.
    assert(TC.getDeclCaptureKind(capture,
                                 TC.getCaptureTypeExpansionContext(constant))
             == CaptureKind::Constant);

    auto value = captureArgs[i].copy(SGF, loc);
    return SGF.emitActorInstanceIsolation(loc, value, isolatedVarType);
  }

  // The capture not being a lowered capture can happen in global code.
  auto value = SGF.emitRValueForDecl(loc, isolatedCapture, isolatedVarType,
                                     AccessSemantics::Ordinary)
                  .getAsSingleValue(SGF, loc);
  return SGF.emitActorInstanceIsolation(loc, value, isolatedVarType);
}

ManagedValue
SILGenFunction::emitClosureIsolation(SILLocation loc, SILDeclRef constant,
                                     ArrayRef<ManagedValue> captures) {
  auto isolation = getClosureIsolationInfo(constant);
  switch (isolation) {
  case ActorIsolation::Unspecified:
  case ActorIsolation::Nonisolated:
  case ActorIsolation::NonisolatedUnsafe:
    return emitNonIsolatedIsolation(loc);

  case ActorIsolation::Erased:
    llvm_unreachable("closures cannot directly have erased isolation");

  case ActorIsolation::GlobalActor:
    return emitGlobalActorIsolation(loc,
             isolation.getGlobalActor()->getCanonicalType());

  case ActorIsolation::ActorInstance: {
    // This should always be a capture.  That's not expressed super-cleanly
    // in ActorIsolation, unfortunately.
    assert(isolation.getActorInstanceParameter() == 0);
    auto capture = isolation.getActorInstance();
    assert(capture);
    return emitLoadOfCaptureIsolation(*this, loc, capture, constant, captures);
  }
  }
  llvm_unreachable("bad kind");
}

ExecutorBreadcrumb
SILGenFunction::emitHopToTargetActor(SILLocation loc,
                                     std::optional<ActorIsolation> maybeIso,
                                     std::optional<ManagedValue> maybeSelf) {
  if (!maybeIso)
    return ExecutorBreadcrumb();

  if (auto executor = emitExecutor(loc, *maybeIso, maybeSelf)) {
    return emitHopToTargetExecutor(loc, *executor);
  } else {
    return ExecutorBreadcrumb();
  }
}

ExecutorBreadcrumb SILGenFunction::emitHopToTargetExecutor(
    SILLocation loc, SILValue executor) {
  // Record that we need to hop back to the current executor.
  auto breadcrumb = ExecutorBreadcrumb(true);
  B.createHopToExecutor(RegularLocation::getDebugOnlyLocation(loc, getModule()),
                        executor, /*mandatory*/ false);
  return breadcrumb;
}

std::optional<SILValue>
SILGenFunction::emitExecutor(SILLocation loc, ActorIsolation isolation,
                             std::optional<ManagedValue> maybeSelf) {
  switch (isolation.getKind()) {
  case ActorIsolation::Unspecified:
  case ActorIsolation::Nonisolated:
  case ActorIsolation::NonisolatedUnsafe:
    return std::nullopt;

  case ActorIsolation::Erased:
    llvm_unreachable("executor emission for erased isolation is unimplemented");

  case ActorIsolation::ActorInstance: {
    // "self" here means the actor instance's "self" value.
    assert(maybeSelf.has_value() && "actor-instance but no self provided?");
    auto self = maybeSelf.value();
    return emitLoadActorExecutor(loc, self);
  }

  case ActorIsolation::GlobalActor:
    return emitLoadGlobalActorExecutor(isolation.getGlobalActor());
  }
  llvm_unreachable("covered switch");
}

void SILGenFunction::emitHopToActorValue(SILLocation loc, ManagedValue actor) {
  // TODO: can the type system enforce this async requirement?
  if (!F.isAsync()) {
    llvm::report_fatal_error("Builtin.hopToActor must be in an async function");
  }
  auto isolation =
      getActorIsolationOfContext(FunctionDC, [](AbstractClosureExpr *CE) {
        return CE->getActorIsolation();
      });
  if (isolation != ActorIsolation::Nonisolated &&
      isolation != ActorIsolation::NonisolatedUnsafe &&
      isolation != ActorIsolation::Unspecified) {
    // TODO: Explicit hop with no hop-back should only be allowed in nonisolated
    // async functions. But it needs work for any closure passed to
    // Task.detached, which currently has unspecified isolation.
    llvm::report_fatal_error(
      "Builtin.hopToActor must be in an actor-independent function");
  }
  SILValue executor = emitLoadActorExecutor(loc, actor);
  B.createHopToExecutor(RegularLocation::getDebugOnlyLocation(loc, getModule()),
                        executor, /*mandatory*/ true);
}

static bool isCheckExpectedExecutorIntrinsicAvailable(SILGenModule &SGM) {
  auto checkExecutor = SGM.getCheckExpectedExecutor();
  if (!checkExecutor)
    return false;

  // Forego a check if instrinsic is unavailable, this could happen
  // in main-actor context.
  auto &C = checkExecutor->getASTContext();
  if (!C.LangOpts.DisableAvailabilityChecking) {
    auto deploymentAvailability = AvailabilityContext::forDeploymentTarget(C);
    auto declAvailability =
        AvailabilityInference::availableRange(checkExecutor, C);
    return deploymentAvailability.isContainedIn(declAvailability);
  }

  return true;
}

void SILGenFunction::emitPreconditionCheckExpectedExecutor(
    SILLocation loc, ActorIsolation isolation,
    std::optional<ManagedValue> actorSelf) {
  if (!isCheckExpectedExecutorIntrinsicAvailable(SGM))
    return;

  auto executor = emitExecutor(loc, isolation, actorSelf);
  assert(executor);
  emitPreconditionCheckExpectedExecutor(loc, *executor);
}

void SILGenFunction::emitPreconditionCheckExpectedExecutor(
    SILLocation loc, SILValue executorOrActor) {
  if (!isCheckExpectedExecutorIntrinsicAvailable(SGM))
    return;

  // We don't want the debugger to step into these.
  loc.markAutoGenerated();

  // Get the executor.
  SILValue executor = B.createExtractExecutor(loc, executorOrActor);

  // Call the library function that performs the checking.
  auto args = emitSourceLocationArgs(loc.getSourceLoc(), loc);

  emitApplyOfLibraryIntrinsic(
      loc, SGM.getCheckExpectedExecutor(), SubstitutionMap(),
      {args.filenameStartPointer, args.filenameLength, args.filenameIsAscii,
       args.line, ManagedValue::forObjectRValueWithoutOwnership(executor)},
      SGFContext());
}

bool SILGenFunction::unsafelyInheritsExecutor() {
  if (auto fn = dyn_cast<AbstractFunctionDecl>(FunctionDC))
    return fn->getAttrs().hasAttribute<UnsafeInheritExecutorAttr>();
  return false;
}

void ExecutorBreadcrumb::emit(SILGenFunction &SGF, SILLocation loc) {
  if (mustReturnToExecutor) {
    assert(SGF.ExpectedExecutor || SGF.unsafelyInheritsExecutor());
    if (auto executor = SGF.ExpectedExecutor)
      SGF.B.createHopToExecutor(
          RegularLocation::getDebugOnlyLocation(loc, SGF.getModule()), executor,
          /*mandatory*/ false);
  }
}

SILValue SILGenFunction::emitGetCurrentExecutor(SILLocation loc) {
  assert(ExpectedExecutor && "prolog failed to set up expected executor?");
  return ExpectedExecutor;
}

/// Find the extension on DistributedActor that defines __actorUnownedExecutor.
static ExtensionDecl *findDistributedActorAsActorExtension(
    ProtocolDecl *distributedActorProto, ModuleDecl *module) {
  ASTContext &ctx = distributedActorProto->getASTContext();
  auto name = ctx.getIdentifier("__actorUnownedExecutor");
  auto results = distributedActorProto->lookupDirect(
      name, SourceLoc(),
      NominalTypeDecl::LookupDirectFlags::IncludeAttrImplements);
  for (auto result : results) {
    if (auto var = dyn_cast<VarDecl>(result)) {
      return dyn_cast<ExtensionDecl>(var->getDeclContext());
    }
  }

  return nullptr;
}

ProtocolConformanceRef
SILGenModule::getDistributedActorAsActorConformance(SubstitutionMap subs) {
  ASTContext &ctx = M.getASTContext();
  auto actorProto = ctx.getProtocol(KnownProtocolKind::Actor);
  Type distributedActorType = subs.getReplacementTypes()[0];

  if (!distributedActorAsActorConformance) {
    auto distributedActorProto = ctx.getProtocol(KnownProtocolKind::DistributedActor);
    if (!distributedActorProto)
      return ProtocolConformanceRef();

    auto ext = findDistributedActorAsActorExtension(
        distributedActorProto, M.getSwiftModule());
    if (!ext)
      return ProtocolConformanceRef();

    // Conformance of DistributedActor to Actor.
    auto genericParam = subs.getGenericSignature().getGenericParams()[0];
    distributedActorAsActorConformance = ctx.getNormalConformance(
        Type(genericParam), actorProto, SourceLoc(), ext,
        ProtocolConformanceState::Incomplete, /*isUnchecked=*/false,
        /*isPreconcurrency=*/false);
  }

  return ProtocolConformanceRef(
      actorProto,
      ctx.getSpecializedConformance(distributedActorType,
                                    distributedActorAsActorConformance,
                                    subs));
}

ManagedValue
SILGenFunction::emitDistributedActorAsAnyActor(SILLocation loc,
                                           SubstitutionMap distributedActorSubs,
                                               ManagedValue actorValue) {
  ProtocolConformanceRef conformances[1] = {
    SGM.getDistributedActorAsActorConformance(distributedActorSubs)
  };

  // Erase the distributed actor instance into an `any Actor` existential with
  // the special conformance.
  auto &ctx = SGM.getASTContext();
  CanType distributedActorType =
    distributedActorSubs.getReplacementTypes()[0]->getCanonicalType();
  auto &distributedActorTL = getTypeLowering(distributedActorType);
  auto actorProto = ctx.getProtocol(KnownProtocolKind::Actor);
  auto &anyActorTL = getTypeLowering(actorProto->getDeclaredExistentialType());
  return emitExistentialErasure(loc, distributedActorType,
                                distributedActorTL, anyActorTL,
                                ctx.AllocateCopy(conformances),
                                SGFContext(),
                                [actorValue](SGFContext) {
    return actorValue;
  });
}
