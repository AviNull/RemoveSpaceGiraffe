#include "RemoveSpaceGiraffeSubsystem.h"

#include "Creature/FGCreature.h"
#include "FGCreatureSubsystem.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "UObject/SoftObjectPath.h"

// Char_SpaceGiraffe_C lives in FactoryGame content, so it's a Blueprint class with no compile-time
// C++ type. This is its content path; TryLoadClass<AFGCreature> both loads and type-checks it.
const TCHAR* const ARemoveSpaceGiraffeSubsystem::TargetClassPath =
	TEXT("/Game/FactoryGame/Character/Creature/Wildlife/SpaceGiraffe/Char_SpaceGiraffe.Char_SpaceGiraffe_C");

ARemoveSpaceGiraffeSubsystem::ARemoveSpaceGiraffeSubsystem()
{
	PrimaryActorTick.bCanEverTick = false;
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer;
}

void ARemoveSpaceGiraffeSubsystem::BeginPlay()
{
	Super::BeginPlay();
	ActivateRemoval();
}

void ARemoveSpaceGiraffeSubsystem::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DeactivateRemoval();
	Super::EndPlay(EndPlayReason);
}

void ARemoveSpaceGiraffeSubsystem::PostLoadGame_Implementation(const int32 SaveVersion, const int32 GameVersion)
{
	// An in-session "Load Game" doesn't re-run BeginPlay, but the override table is session-only
	// and the newly loaded save may have streamed in different content, so re-run activation here.
	if (HasAuthority())
	{
		ActivateRemoval();
	}
}

void ARemoveSpaceGiraffeSubsystem::ActivateRemoval()
{
	if (!HasAuthority())
	{
		return;
	}

	if (!ValidateAndResolveTargetClass())
	{
		// Validation failed: no destructive action, no handler registration, no override.
		return;
	}

	// Session-only on the native side, so this must be re-applied on every activation.
	RegisterCreatureClassOverride();

	if (!bActivated)
	{
		RegisterActorSpawnedHandler();
		RegisterLevelStreamingHandler();
		bActivated = true;
	}

	// Sweep everything already loaded (persistent level plus any cells already streamed in).
	SweepLevel(nullptr);
}

void ARemoveSpaceGiraffeSubsystem::DeactivateRemoval()
{
	UnregisterActorSpawnedHandler();
	UnregisterLevelStreamingHandler();
	ClearDeferredRemoval();
	bActivated = false;
}

bool ARemoveSpaceGiraffeSubsystem::ValidateAndResolveTargetClass()
{
	if (bTargetClassValid && ResolvedTargetClass)
	{
		return true;
	}

	const FSoftClassPath ClassPath(TargetClassPath);
	UClass* LoadedClass = ClassPath.TryLoadClass<AFGCreature>();
	if (!LoadedClass)
	{
		UE_LOG(LogTemp, Error,
			TEXT("RemoveSpaceGiraffe: failed to resolve target class '%s' as a subclass of AFGCreature; ")
			TEXT("aborting all removal and spawn-prevention logic until it can be resolved."),
			TargetClassPath);
		bTargetClassValid = false;
		return false;
	}

	ResolvedTargetClass = LoadedClass;
	bTargetClassValid = true;
	UE_LOG(LogTemp, Warning, TEXT("RemoveSpaceGiraffe: resolved target class '%s'."), *ResolvedTargetClass->GetName());
	return true;
}

void ARemoveSpaceGiraffeSubsystem::RegisterCreatureClassOverride()
{
	if (!bTargetClassValid || !ResolvedTargetClass)
	{
		return;
	}

	AFGCreatureSubsystem* CreatureSubsystem = AFGCreatureSubsystem::Get(GetWorld());
	if (!CreatureSubsystem)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("RemoveSpaceGiraffe: creature subsystem not available yet; native spawn-prevention ")
			TEXT("override was not (re-)registered this pass. The actor-spawn and level-streaming ")
			TEXT("backstops remain active regardless."));
		return;
	}

	// Overriding to a null class tells the native spawner system not to spawn this creature class;
	// DESTROY disposes of any instances the creature subsystem already tracks.
	CreatureSubsystem->AddCreatureClassOverride(ResolvedTargetClass, nullptr, ECreatureReplaceAction::DESTROY);
	UE_LOG(LogTemp, Warning,
		TEXT("RemoveSpaceGiraffe: registered native override suppressing spawns of '%s'."),
		*ResolvedTargetClass->GetName());
}

void ARemoveSpaceGiraffeSubsystem::RegisterActorSpawnedHandler()
{
	UWorld* World = GetWorld();
	if (World && !ActorSpawnedHandle.IsValid())
	{
		ActorSpawnedHandle = World->AddOnActorSpawnedHandler(
			FOnActorSpawned::FDelegate::CreateUObject(this, &ARemoveSpaceGiraffeSubsystem::HandleActorSpawned));
	}
}

void ARemoveSpaceGiraffeSubsystem::UnregisterActorSpawnedHandler()
{
	if (UWorld* World = GetWorld(); World && ActorSpawnedHandle.IsValid())
	{
		World->RemoveOnActorSpawnedHandler(ActorSpawnedHandle);
	}
	ActorSpawnedHandle.Reset();
}

void ARemoveSpaceGiraffeSubsystem::HandleActorSpawned(AActor* SpawnedActor)
{
	if (!HasAuthority() || !bTargetClassValid || !ResolvedTargetClass || !IsValid(SpawnedActor))
	{
		return;
	}

	if (!SpawnedActor->IsA(ResolvedTargetClass))
	{
		return;
	}

	// Backstop only: this should be rare, since the native override already asks the spawner not
	// to create this class. Defer the actual destroy to next tick so we never destroy an actor
	// while it's still mid-construction.
	UE_LOG(LogTemp, Warning,
		TEXT("RemoveSpaceGiraffe: caught a spawn of '%s' that slipped past the native override; queuing for removal."),
		*SpawnedActor->GetPathName());
	PendingRemovals.Add(SpawnedActor);
	ScheduleDeferredRemovalIfNeeded();
}

void ARemoveSpaceGiraffeSubsystem::RegisterLevelStreamingHandler()
{
	if (!LevelAddedHandle.IsValid())
	{
		LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddUObject(
			this, &ARemoveSpaceGiraffeSubsystem::HandleLevelAddedToWorld);
	}
}

void ARemoveSpaceGiraffeSubsystem::UnregisterLevelStreamingHandler()
{
	if (LevelAddedHandle.IsValid())
	{
		FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedHandle);
		LevelAddedHandle.Reset();
	}
}

void ARemoveSpaceGiraffeSubsystem::HandleLevelAddedToWorld(ULevel* InLevel, UWorld* InWorld)
{
	if (!HasAuthority() || !bTargetClassValid || !ResolvedTargetClass || InWorld != GetWorld() || !InLevel)
	{
		return;
	}

	// World Partition can stream in cells containing instances that were placed directly in the
	// level rather than spawned by a creature spawner, so these need their own sweep. Queue them
	// through the deferred path rather than destroying immediately, since actors in a level that
	// has just been added to the world may not have finished initializing yet.
	int32 QueuedCount = 0;
	const TArray<TObjectPtr<AActor>> LevelActors = InLevel->Actors;
	for (AActor* Actor : LevelActors)
	{
		if (IsValid(Actor) && Actor->IsA(ResolvedTargetClass))
		{
			PendingRemovals.Add(Actor);
			++QueuedCount;
		}
	}

	if (QueuedCount > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("RemoveSpaceGiraffe: queued %d instance(s) found in newly streamed level '%s'."),
			QueuedCount, *InLevel->GetOutermost()->GetName());
		ScheduleDeferredRemovalIfNeeded();
	}
}

int32 ARemoveSpaceGiraffeSubsystem::SweepLevel(ULevel* Level)
{
	// --- Validate ---
	if (!HasAuthority() || !bTargetClassValid || !ResolvedTargetClass)
	{
		return 0;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return 0;
	}

	// --- Prepare: snapshot candidates before destroying anything. ---
	TArray<TWeakObjectPtr<AActor>> Candidates;
	if (Level)
	{
		const TArray<TObjectPtr<AActor>> LevelActors = Level->Actors;
		for (AActor* Actor : LevelActors)
		{
			if (IsValid(Actor) && Actor->IsA(ResolvedTargetClass))
			{
				Candidates.Add(Actor);
			}
		}
	}
	else
	{
		for (TActorIterator<AActor> It(World, ResolvedTargetClass); It; ++It)
		{
			if (IsValid(*It))
			{
				Candidates.Add(*It);
			}
		}
	}

	if (Candidates.Num() == 0)
	{
		return 0;
	}

	// --- Apply ---
	int32 DestroyedCount = 0;
	for (const TWeakObjectPtr<AActor>& WeakActor : Candidates)
	{
		AActor* Actor = WeakActor.Get();
		if (!IsValid(Actor))
		{
			continue;
		}
		if (Actor->Destroy())
		{
			++DestroyedCount;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("RemoveSpaceGiraffe: failed to destroy instance '%s'."), *Actor->GetPathName());
		}
	}

	// --- Verify ---
	int32 RemainingCount = 0;
	for (const TWeakObjectPtr<AActor>& WeakActor : Candidates)
	{
		if (IsValid(WeakActor.Get()))
		{
			++RemainingCount;
		}
	}
	if (RemainingCount > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("RemoveSpaceGiraffe: %d instance(s) survived this pass; will be retried on the next sweep."),
			RemainingCount);
	}

	// --- Commit saved state ---
	if (DestroyedCount > 0)
	{
		TotalInstancesRemoved += DestroyedCount;
		UE_LOG(LogTemp, Warning,
			TEXT("RemoveSpaceGiraffe: removed %d instance(s) (total removed this save: %d)."),
			DestroyedCount, TotalInstancesRemoved);
	}

	return DestroyedCount;
}

void ARemoveSpaceGiraffeSubsystem::ScheduleDeferredRemovalIfNeeded()
{
	if (bDeferredRemovalScheduled)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	bDeferredRemovalScheduled = true;
	DeferredRemovalTimerHandle = World->GetTimerManager().SetTimerForNextTick(
		FTimerDelegate::CreateUObject(this, &ARemoveSpaceGiraffeSubsystem::ProcessDeferredRemoval));
}

void ARemoveSpaceGiraffeSubsystem::ProcessDeferredRemoval()
{
	bDeferredRemovalScheduled = false;
	DeferredRemovalTimerHandle = FTimerHandle();

	// --- Validate ---
	if (!HasAuthority() || !bTargetClassValid || !ResolvedTargetClass)
	{
		PendingRemovals.Reset();
		return;
	}

	// --- Prepare ---
	const TSet<TWeakObjectPtr<AActor>> Snapshot = MoveTemp(PendingRemovals);
	PendingRemovals.Reset();

	// --- Apply ---
	int32 DestroyedCount = 0;
	for (const TWeakObjectPtr<AActor>& WeakActor : Snapshot)
	{
		AActor* Actor = WeakActor.Get();
		if (!IsValid(Actor) || !Actor->IsA(ResolvedTargetClass))
		{
			continue;
		}
		if (Actor->Destroy())
		{
			++DestroyedCount;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("RemoveSpaceGiraffe: deferred destroy failed for '%s'."), *Actor->GetPathName());
		}
	}

	// --- Verify ---
	int32 RemainingCount = 0;
	for (const TWeakObjectPtr<AActor>& WeakActor : Snapshot)
	{
		if (IsValid(WeakActor.Get()))
		{
			++RemainingCount;
		}
	}
	if (RemainingCount > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("RemoveSpaceGiraffe: %d queued instance(s) still present after the deferred removal pass."),
			RemainingCount);
	}

	// --- Commit saved state ---
	if (DestroyedCount > 0)
	{
		TotalInstancesRemoved += DestroyedCount;
		UE_LOG(LogTemp, Warning,
			TEXT("RemoveSpaceGiraffe: deferred pass removed %d instance(s) (total removed this save: %d)."),
			DestroyedCount, TotalInstancesRemoved);
	}
}

void ARemoveSpaceGiraffeSubsystem::ClearDeferredRemoval()
{
	if (UWorld* World = GetWorld(); World && DeferredRemovalTimerHandle.IsValid())
	{
		World->GetTimerManager().ClearTimer(DeferredRemovalTimerHandle);
	}
	DeferredRemovalTimerHandle = FTimerHandle();
	bDeferredRemovalScheduled = false;
	PendingRemovals.Reset();
}
