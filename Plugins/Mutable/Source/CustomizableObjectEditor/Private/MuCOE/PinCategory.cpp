// Copyright Epic Games, Inc. All Rights Reserved.

#include "PinCategory.h"

#include "MuCO/ICustomizableObjectModule.h"
#include "MuCOE/EdGraphSchema_CustomizableObject.h"


FName GetPinCategoryName(const FName& PinCategory)
{
	if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Object)
	{
		return TEXT("Object");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Component)
	{
		return TEXT("Component");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Material)
	{
		return TEXT("Material");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Modifier)
	{
		return TEXT("Modifier");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Mesh)
	{
		return TEXT("Mesh");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Layout)
	{
		return TEXT("Layout");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Image)
	{
		return TEXT("Texture");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_PassThroughImage)
	{
		return TEXT("PassThrough Texture");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Projector)
	{
		return TEXT("Projector");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_GroupProjector)
	{
		return TEXT("Group Projector");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Color)
	{
		return TEXT("Color");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Float)
	{
		return TEXT("Float");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Bool)
	{
		return TEXT("Bool");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Enum)
	{
		return TEXT("Enum");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Stack)
	{
		return TEXT("Stack");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_MaterialAsset)
	{
		return TEXT("Table Material");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Wildcard)
	{
		return TEXT("Wildcard");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_PoseAsset)
	{
		return TEXT("PoseAsset");
	}
	else if (PinCategory == UEdGraphSchema_CustomizableObject::PC_Transform)
	{
		return TEXT("Transform");
	}
	else
	{
		for (const FRegisteredCustomizableObjectPinType& PinType : ICustomizableObjectModule::Get().GetExtendedPinTypes())
		{
			if (PinType.PinType.Name == PinCategory)
			{
				return PinType.PinType.Name;
			}
		}

		// Need to fail gracefully here in case a plugin that was active when this graph was
		// created is no longer loaded.
		return TEXT("Unknown");
	}
}

