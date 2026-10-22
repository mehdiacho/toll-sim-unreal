#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "TollCookList.generated.h"

/**
 * Hard references to the pack meshes the scene draws, so the cook takes exactly those.
 * The scene finds them by folder at runtime and never loads this; it only exists for the cook.
 * Written by Scripts/make_cook_list.py as /Game/Toll/DA_CookList.
 */
UCLASS(BlueprintType)
class TOLLSIMUE_API UTollCookList : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Cook")
	TArray<TObjectPtr<UObject>> Assets;
};
