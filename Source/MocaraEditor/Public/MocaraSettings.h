#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MocaraSettings.generated.h"

class AActor;

UCLASS(Config=Editor, DefaultConfig, meta=(DisplayName="Mocara"))
class UMocaraSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UMocaraSettings()
	{
		CategoryName = TEXT("Plugins");
		SectionName = TEXT("Mocara");
	}

	/**
	 * Where the editor talks to the sidecar. The port is forwarded to run_sidecar.sh as
	 * MOCARA_PORT when the editor launches it, so changing this moves both ends. Only
	 * loopback is supported -- the sidecar deliberately binds 127.0.0.1 and nothing else.
	 */
	UPROPERTY(EditAnywhere, Config, Category="Sidecar")
	FString SidecarUrl = TEXT("http://127.0.0.1:8765");

	/** Launch the WSL Kimodo sidecar from the editor when Mocara starts. */
	UPROPERTY(EditAnywhere, Config, Category="Sidecar")
	bool bAutoStartSidecar = true;

	UPROPERTY(EditAnywhere, Config, Category="Sidecar")
	FString WslDistro = TEXT("Ubuntu");

	/**
	 * Windows path to run_sidecar.sh. Leave empty to use the copy that ships inside the
	 * plugin, which is what lets Mocara be dropped into any project.
	 */
	UPROPERTY(EditAnywhere, Config, Category="Sidecar")
	FString SidecarScriptPath;

	/**
	 * Optional Windows path to another root that holds Sidecar/ (the Python package).
	 * Passed to the scripts as MOCARA_ROOT. Leave empty to use the runtime packaged
	 * inside the installed Mocara plugin. This is primarily a sidecar-development hook.
	 */
	UPROPERTY(EditAnywhere, Config, Category="Sidecar")
	FString SidecarRoot;

	/**
	 * Write generated assets to disk as soon as they are created. Off means they exist
	 * only in memory and are lost if the editor closes without a manual Save All.
	 */
	UPROPERTY(EditAnywhere, Config, Category="Assets")
	bool bAutoSaveGenerated = true;

	UPROPERTY(EditAnywhere, Config, Category="Assets")
	FString GeneratedPath = TEXT("/Game/Mocara/Generated");

	UPROPERTY(EditAnywhere, Config, Category="Assets")
	FString RetargetPath = TEXT("/Game/Mocara/Retarget");

	/**
	 * Skeletal Mesh that receives generated motion. Select SKM_Manny, an assembled
	 * MetaHuman's *_body mesh, or another mesh supported by a Mocara target profile.
	 * Leave empty to keep the existing automatic UE5 mannequin discovery behavior.
	 */
	UPROPERTY(EditAnywhere, Config, Category="Retarget", meta=(AllowedClasses="/Script/Engine.SkeletalMesh"))
	FSoftObjectPath TargetMesh;

	/**
	 * Optional assembled character shown in Animation Lab while its compatible Body
	 * component follows the editable target mesh. When empty, Mocara discovers a matching
	 * Blueprint under /Game/MetaHumans (for example BP_Kellan in this project).
	 */
	UPROPERTY(EditAnywhere, Config, Category="Preview")
	TSoftClassPtr<AActor> PreviewCharacterClass;

	/**
	 * How strongly the retarget pins the target's feet to the floor.
	 *
	 * At 1.0 ground penetration is removed entirely and planted-foot slide drops from
	 * 1.35 to 0.48 cm/frame (worst-case spike 12.9 -> 1.7), at the cost of pulling the
	 * knees ~8 deg away from the source pose -- unavoidable when the target's proportions
	 * differ from the source's. At 0.0 the legs copy the source exactly and the feet
	 * sink through the floor. 0.5 is a middle ground, but the worst slide spike returns.
	 */
	UPROPERTY(EditAnywhere, Config, Category="Retarget", meta=(ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0"))
	float FootPlantingStrength = 1.0f;

	/**
	 * Include the character mesh in exported FBX files. Off by default, which gives an
	 * animation-only FBX (skeleton + curves, no geometry) -- the usual hand-off when the
	 * recipient already has the character. Turn on to ship the mesh in the same file.
	 */
	UPROPERTY(EditAnywhere, Config, Category="Export")
	bool bExportMeshWithAnimation = false;

	/** Legacy setting retained so existing Mocara project configuration keeps working. */
	UPROPERTY(Config, meta=(DeprecatedProperty, DeprecationMessage="Use Target Mesh instead."))
	FSoftObjectPath MannyMesh;

	/** Legacy search paths retained for existing projects. */
	UPROPERTY(Config, meta=(DeprecatedProperty, DeprecationMessage="Use Target Mesh instead."))
	TArray<FSoftObjectPath> ExtraMannySearchPaths;
};
