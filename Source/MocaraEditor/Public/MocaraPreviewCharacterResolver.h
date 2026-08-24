#pragma once

#include "CoreMinimal.h"

class AActor;
class USkeletalMesh;
class USkeletalMeshComponent;

/** Finds an assembled character whose body can follow the Animation Lab edit proxy. */
struct MOCARAEDITOR_API FMocaraPreviewCharacterResolver
{
	/** PreferredClass is used when compatible; otherwise /Game/MetaHumans is searched. */
	static UClass* FindCompatibleClass(
		USkeletalMesh* TargetMesh,
		UClass* PreferredClass,
		FName& OutBodyComponentName);

	/** Resolves the live body component after the assembled actor has been spawned. */
	static USkeletalMeshComponent* FindCompatibleBodyComponent(
		AActor* Actor,
		USkeletalMesh* TargetMesh,
		FName PreferredComponentName = NAME_None);
};
