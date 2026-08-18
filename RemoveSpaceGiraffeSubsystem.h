#pragma once

#include "CoreMinimal.h"
#include "FGSaveInterface.h"
#include "Subsystem/ModSubsystem.h"
#include "RemoveSpaceGiraffeSubsystem.generated.h"

/**
 * Removes every existing Char_SpaceGiraffe_C instance from the world and prevents the class from
 * spawning again, for as long as this subsystem is active. Server-authoritative: all destructive
 * work is gated on HasAuthority(), and the subsystem is configured to spawn on the server only
 * (see ReplicationPolicy in the constructor), so it runs correctly on dedicated servers and does
 * nothing on remote clients.
 *
 * Handling is persistent rather than one-shot because World Partition streams new cells into the
 * world over time, which can bring in both fresh spawn attempts and previously-unloaded, already
 * -placed instances well after the level first starts. Three complementary mechanisms cover this:
 *   1. A native creature-class override (AFGCreatureSubsystem::AddCreatureClassOverride) tells the
 *      game's own spawner system not to spawn the class, and to dispose of instances it already
 *      tracks. This table is session-only (not saved by the base game), so it is re-registered on
 *      every activation (new game, loaded game, or in-session reload).
 *   2. An AActor spawn handler destroys any instance that is created despite the override, as a
 *      backstop against spawn paths the override doesn't cover.
 *   3. A FWorldDelegates::LevelAddedToWorld handler sweeps every level as it streams in, catching
 *      already-placed instances that were never spawned through the creature subsystem at all.
 *
 * Every removal pass follows the same transaction shape: validate authority and the resolved
 * target class, prepare a snapshot of candidate actors, apply destruction to that snapshot, verify
 * nothing survived, then commit the running total to saved state. No destructive action is taken
 * until the target class has been validated.
 */
UCLASS()
class REMOVESPACEGIRAFFE_API ARemoveSpaceGiraffeSubsystem : public AModSubsystem, public IFGSaveInterface
{
	GENERATED_BODY()

public:
	ARemoveSpaceGiraffeSubsystem();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// Begin IFGSaveInterface
	virtual void PreSaveGame_Implementation(int32 SaveVersion, int32 GameVersion) override {}
	virtual void PostSaveGame_Implementation(int32 SaveVersion, int32 GameVersion) override {}
	virtual void PreLoadGame_Implementation(int32 SaveVersion, int32 GameVersion) override {}
	virtual void PostLoadGame_Implementation(int32 SaveVersion, int32 GameVersion) override;
	virtual void GatherDependencies_Implementation(TArray<UObject*>& OutDependentObjects) override {}
	virtual bool NeedTransform_Implementation() override { return false; }
	virtual bool ShouldSave_Implementation() const override { return true; }
	// End IFGSaveInterface

private:
	/** Re-registers the native override and handlers (idempotent), then runs a full-world sweep. */
	void ActivateRemoval();

	/** Unregisters handlers and clears pending state. Safe to call even if never activated. */
	void DeactivateRemoval();

	/** Resolves and validates the target creature class exactly once; cached afterwards. */
	bool ValidateAndResolveTargetClass();

	/** Tells the native creature subsystem to stop spawning the target class and to destroy any
	 *  instances it already tracks. Must be called every activation since the override isn't saved. */
	void RegisterCreatureClassOverride();

	void RegisterActorSpawnedHandler();
	void UnregisterActorSpawnedHandler();
	void HandleActorSpawned(AActor* SpawnedActor);

	void RegisterLevelStreamingHandler();
	void UnregisterLevelStreamingHandler();
	void HandleLevelAddedToWorld(class ULevel* InLevel, class UWorld* InWorld);

	/** Transaction-shaped sweep: validate -> prepare -> apply -> verify -> commit saved state.
	 *  Sweeps the whole world when Level is null, or just the given level otherwise. Returns the
	 *  number of instances destroyed. */
	int32 SweepLevel(class ULevel* Level);

	void ScheduleDeferredRemovalIfNeeded();
	void ProcessDeferredRemoval();
	void ClearDeferredRemoval();

	/** Soft class path of Char_SpaceGiraffe_C, resolved and validated at runtime rather than
	 *  referenced at compile time since it is a content-only Blueprint class. */
	static const TCHAR* const TargetClassPath;

	UPROPERTY()
	TSubclassOf<class AFGCreature> ResolvedTargetClass;

	bool bTargetClassValid = false;
	bool bActivated = false;

	FDelegateHandle LevelAddedHandle;
	FDelegateHandle ActorSpawnedHandle;

	TSet<TWeakObjectPtr<AActor>> PendingRemovals;
	FTimerHandle DeferredRemovalTimerHandle;
	bool bDeferredRemovalScheduled = false;

	/** Cumulative instances removed across this save's lifetime. Committed as the final step of
	 *  every transaction; purely informational, since removal itself is re-run on every activation
	 *  rather than relying on this value for correctness. */
	UPROPERTY(SaveGame)
	int32 TotalInstancesRemoved = 0;
};
