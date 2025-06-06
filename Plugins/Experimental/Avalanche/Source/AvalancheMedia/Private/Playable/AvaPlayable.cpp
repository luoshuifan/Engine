// Copyright Epic Games, Inc. All Rights Reserved.

#include "Playable/AvaPlayable.h"

#include "AvaSequence.h"
#include "AvaSequencePlayer.h"
#include "Broadcast/AvaBroadcast.h"
#include "Controller/RCCustomControllerUtilities.h"
#include "Engine/Engine.h"
#include "Framework/AvaSoftAssetPtr.h"
#include "IAvaSceneInterface.h"
#include "IAvaSequenceProvider.h"
#include "Playable/AvaPlayableGroup.h"
#include "Playable/AvaPlayableRemoteControl.h"
#include "Playable/AvaPlayableRemoteControlValues.h"
#include "Playable/Playables/AvaPlayableLevelStreaming.h"
#include "Playable/Playables/AvaPlayableRemoteProxy.h"
#include "Playback/AvaPlaybackUtils.h"
#include "RCVirtualProperty.h"
#include "RemoteControlPreset.h"

DEFINE_LOG_CATEGORY(LogAvaPlayable);

#define LOCTEXT_NAMESPACE "AvaPlayable"

// Transition logic and Scene State requires the RC controller values to be updated.
static TAutoConsoleVariable<bool> CVarUpdateRCControllerValues(
	TEXT("MotionDesignPlayable.UpdateRCControllerValues"),
	true,
	TEXT("Set the controller values without running the behaviors."),
	ECVF_Default);

// Some "special" controllers, such as texture controller require the behavior to be executed.
static TAutoConsoleVariable<bool> CVarExecuteSpecialRCControllerBehavior(
	TEXT("MotionDesignPlayable.ExecuteSpecialRCControllerBehavior"),
	true,
	TEXT("Execute the special controller behaviors (such as texture controllers)."),
	ECVF_Default);

namespace UE::AvaPlayable::Private
{
	bool ShouldCreateLocalPlayable(const FName& InChannelName, const UAvaBroadcast& InBroadcast)
	{
		const FAvaBroadcastOutputChannel& Channel = InBroadcast.GetCurrentProfile().GetChannel(InChannelName);

		// If there is no broadcast channel defined, like for preview (by default), then this is a local playable.
		if (!Channel.IsValidChannel())
		{
			return true;
		}

		// For non-preview, the commands will be executed locally if the channel has at least one local outputs or no outputs.
		// The "no outputs" condition is considered valid. Empty channels run locally.
		if (Channel.HasAnyLocalMediaOutputs() || Channel.GetMediaOutputs().IsEmpty())
		{
			return true;
		}
		
		return false;
	}

	bool HasRemoteOutputs(const FName& InChannelName, const UAvaBroadcast& InBroadcast)
	{
		const FAvaBroadcastOutputChannel& Channel = InBroadcast.GetCurrentProfile().GetChannel(InChannelName);
		return Channel.IsValidChannel() && Channel.HasAnyRemoteMediaOutputs();
	}

	FString GetPrettyPlayableInfo(const UAvaPlayable* InPlayable)
	{
		if (InPlayable)
		{
			return FString::Printf(TEXT("Id:%s, Asset:%s, Status:%s"),
				*InPlayable->GetInstanceId().ToString(),
				*InPlayable->GetSourceAssetPath().GetAssetName(),
				*AvaPlayback::Utils::StaticEnumToString(InPlayable->GetPlayableStatus()));
		}
		return TEXT("(nullptr)");
	}
	
	FString GetPrettySequenceInfo(const UAvaSequence* InSequence)
	{
		if (InSequence)
		{
			return FString::Printf(TEXT("Name:%s, Label:%s"),
				*InSequence->GetFName().ToString(),
				*InSequence->GetLabel().ToString());
		}
		return TEXT("(nullptr)");
	}		

	FString GetBriefFrameInfo()
	{
		return UE::AvaPlayback::Utils::GetBriefFrameInfo();
	}
	
	FString GetPrettySequenceCommandInfo(EAvaPlaybackAnimAction InAnimAction, const FAvaPlaybackAnimPlaySettings& InAnimPlaySettings)
	{
		return FString::Printf(TEXT("Action:%s, Name:%s"),
			*AvaPlayback::Utils::StaticEnumToString(InAnimAction),
			*InAnimPlaySettings.AnimationName.ToString());
	}
}

UAvaPlayable::FOnSequenceEvent UAvaPlayable::OnSequenceEventDelegate;
UAvaPlayable::FOnTransitionEvent UAvaPlayable::OnTransitionEventDelegate;

UAvaPlayable* UAvaPlayable::Create(UObject* InOuter, const FPlayableCreationInfo& InPlayableInfo)
{
	using namespace UE::AvaPlayable::Private;

	const UAvaBroadcast& Broadcast = UAvaBroadcast::Get();
	
	UAvaPlayable* NewPlayable;
	
	// Forked channels considerations:
	// - The case of forked remote channels is/will be handled internally to the RemoteProxy playable.
	// - The case of forked local and remote channels will lead to a local playable and a remote proxy playable.
	//   It would require wrapping the playables in a composite (or facade?) proxy. TODO
	
	if (ShouldCreateLocalPlayable(InPlayableInfo.ChannelName, Broadcast))
	{
		// For the moment, remote outputs will be ignored.
		if (HasRemoteOutputs(InPlayableInfo.ChannelName, Broadcast))
		{
			UE_LOG(LogAvaPlayable, Error, TEXT("Forked Channels with both local and remote outputs are not supported in this version. Only local instance will be created."));
		}
		
		NewPlayable = CreateLocalPlayable(InOuter, InPlayableInfo);
	}
	else
	{
		// Purely remote channel.
		NewPlayable = CreateRemoteProxyPlayable(InOuter, InPlayableInfo);
	}

	// Finish the setup.
	if (NewPlayable && !NewPlayable->InitPlayable(InPlayableInfo))
	{
		// final setup may fail, in this case the playable is discarded.
		return nullptr;
	}

	return NewPlayable;
}

const FSoftObjectPath& UAvaPlayable::GetSourceAssetPath() const
{
	static const FSoftObjectPath Empty;
	return Empty;
}

EAvaPlayableCommandResult UAvaPlayable::ExecuteAnimationCommand(EAvaPlaybackAnimAction InAnimAction, const FAvaPlaybackAnimPlaySettings& InAnimPlaySettings)
{
	using namespace UE::AvaPlayable::Private;

	const EAvaPlayableStatus PlayableStatus = GetPlayableStatus();

	if (PlayableStatus == EAvaPlayableStatus::Unknown
		|| PlayableStatus == EAvaPlayableStatus::Error
		|| PlayableStatus == EAvaPlayableStatus::Unloaded)
	{
		UE_LOG(LogAvaPlayable, Verbose,
			TEXT("%s Playable {%s} -> Discarding Sequence Command: {%s}."),
			*GetBriefFrameInfo(), *GetPrettyPlayableInfo(this), *GetPrettySequenceCommandInfo(InAnimAction, InAnimPlaySettings));
	
		// Discard the command
		return EAvaPlayableCommandResult::ErrorDiscard;
	}

	// Asset status must be visible to run the animation commands.
	// If not visible, the components are not yet added to the world and animations won't execute.
	if (PlayableStatus != EAvaPlayableStatus::Visible)
	{
		UE_LOG(LogAvaPlayable, Verbose,
			TEXT("%s Playable {%s} -> ReQueueing Sequence Command: {%s}."),
			*GetBriefFrameInfo(), *GetPrettyPlayableInfo(this), *GetPrettySequenceCommandInfo(InAnimAction, InAnimPlaySettings));
		
		// Keep the command in the queue for next tick.
		return EAvaPlayableCommandResult::KeepPending;
	}

	const IAvaSceneInterface* Scene = GetSceneInterface();
	if (!Scene)
	{
		return EAvaPlayableCommandResult::ErrorDiscard;
	}
	
	IAvaSequencePlaybackObject* PlaybackObject = Scene->GetPlaybackObject();
	
	if (!PlaybackObject)
	{
		return EAvaPlayableCommandResult::ErrorDiscard;
	}

	const IAvaSequenceProvider* SequenceProvider = Scene->GetSequenceProvider();

	if (!SequenceProvider)
	{
		return EAvaPlayableCommandResult::ErrorDiscard;
	}

	UE_LOG(LogAvaPlayable, Verbose,
		TEXT("%s Playable {%s} -> Executing Sequence Command: {%s}."),
		*GetBriefFrameInfo(), *GetPrettyPlayableInfo(this), *GetPrettySequenceCommandInfo(InAnimAction, InAnimPlaySettings));
	
	for (const TObjectPtr<UAvaSequence>& Sequence : SequenceProvider->GetSequences())
	{
		// Remark: if the command doesn't specify the sequence name, we run the command on all the sequences.
		if (Sequence && (Sequence->GetFName() == InAnimPlaySettings.AnimationName || InAnimPlaySettings.AnimationName.IsNone()))
		{
			switch (InAnimAction)
			{
			case EAvaPlaybackAnimAction::Play:
				PlaybackObject->PlaySequence(Sequence, InAnimPlaySettings.AsPlayParams());
				break;

			case EAvaPlaybackAnimAction::Continue:
				PlaybackObject->ContinueSequence(Sequence);
				break;

			case EAvaPlaybackAnimAction::Stop:
				PlaybackObject->StopSequence(Sequence);
				break;

			case EAvaPlaybackAnimAction::PreviewFrame:
				PlaybackObject->PreviewFrame(Sequence);
				break;
			}
		}
	}
	
	return EAvaPlayableCommandResult::Executed;
}

EAvaPlayableCommandResult UAvaPlayable::UpdateRemoteControlCommand(const TSharedRef<FAvaPlayableRemoteControlValues>& InRemoteControlValues)
{
	const EAvaPlayableStatus PlayableStatus = GetPlayableStatus();

	if (PlayableStatus == EAvaPlayableStatus::Unknown
		|| PlayableStatus == EAvaPlayableStatus::Error
		|| PlayableStatus == EAvaPlayableStatus::Unloaded)
	{
		using namespace UE::AvaPlayable::Private;
		UE_LOG(LogAvaPlayable, Verbose, TEXT("%s Playable {%s} -> Discarding RC Update."), *GetBriefFrameInfo(), *GetPrettyPlayableInfo(this));

		// Discard the command
		return EAvaPlayableCommandResult::ErrorDiscard;
	}

	// Asset status must be visible to run the command.
	// If not visible, the components are not yet added to the world.
	if (PlayableStatus != EAvaPlayableStatus::Visible)
	{
		using namespace UE::AvaPlayable::Private;
		UE_LOG(LogAvaPlayable, Verbose, TEXT("%s Playable {%s} -> ReQueueing RC Update."), *GetBriefFrameInfo(), *GetPrettyPlayableInfo(this));

		// Keep the command in the queue for next tick.
		return EAvaPlayableCommandResult::KeepPending;
	}

	const IAvaSceneInterface* Scene = GetSceneInterface();
	if (!Scene)
	{
		return EAvaPlayableCommandResult::ErrorDiscard;
	}
	
	URemoteControlPreset* RemoteControlPreset = Scene->GetRemoteControlPreset();
	
	if (!IsValid(RemoteControlPreset))
	{
		UE_LOG(LogAvaPlayable, Error,
			TEXT("Remote Control command for asset \"%s\": Remote Control Preset is null."),
			*GetSourceAssetPath().ToString());
		return EAvaPlayableCommandResult::ErrorDiscard;
	}

	LatestRemoteControlValues = InRemoteControlValues;

	using namespace UE::AvaPlayable::Private;
	UE_LOG(LogAvaPlayable, Verbose, TEXT("%s Playable {%s} -> Executing RC Update."), *GetBriefFrameInfo(), *GetPrettyPlayableInfo(this));

	TSet<FGuid> AppliedEntities;
	
	// Apply special controllers that don't work just with entity values.
	const bool bExecuteSpecialControllerBehavior = CVarExecuteSpecialRCControllerBehavior.GetValueOnGameThread();
	const bool bUpdateControllerValues = CVarUpdateRCControllerValues.GetValueOnGameThread();
	if (bExecuteSpecialControllerBehavior || bUpdateControllerValues)
	{
		TArray<URCVirtualPropertyBase*> Controllers = RemoteControlPreset->GetControllers();
		for (URCVirtualPropertyBase* Controller : Controllers)
		{
			using namespace UE::AvaPlayableRemoteControl;
			using namespace UE::RCCustomControllers;

			// Texture Controller Require the Bind Behavior to be executed to setup the texture in external mode.
			if (bExecuteSpecialControllerBehavior &&
				GetCustomControllerTypeName(Controller) == CustomTextureControllerName)
			{
				if (const FAvaPlayableRemoteControlValue* ControllerValue = InRemoteControlValues->GetControllerValue(Controller->Id))
				{
					SetValueOfController(Controller, ControllerValue->Value);
					GetEntitiesControlledByController(RemoteControlPreset, Controller, AppliedEntities);
				}
			}
			else if (bUpdateControllerValues)
			{
				if (const FAvaPlayableRemoteControlValue* ControllerValue = InRemoteControlValues->GetControllerValue(Controller->Id))
				{
					FScopedPushControllerBehavioursEnable PushBehaviorsEnable(Controller, /*bInBehaviorsEnabled*/ false);
					SetValueOfController(Controller, ControllerValue->Value);
				}
			}
		}
	}

	// WYSIWYG (Solution): For the runtime/playback RCP, we don't apply the controllers.
	// We assume the controller actions are already executed in the rundown's managed RCP
	// during page edition and the resulting entity values are already captured.

	// Todo: change this function to support filter predicate.
	//InRemoteControlValues->ApplyEntityValuesToRemoteControlPreset(RemoteControlPreset);

	for (const TWeakPtr<FRemoteControlEntity>& EntityWeakPtr : RemoteControlPreset->GetExposedEntities<FRemoteControlEntity>())
	{
		if (const TSharedPtr<FRemoteControlEntity> Entity = EntityWeakPtr.Pin())
		{
			// Skip properties that are already set by controllers at runtime.
			if (AppliedEntities.Contains(Entity->GetId()))
			{
				continue;
			}
			
			if (const FAvaPlayableRemoteControlValue* EntityValue = InRemoteControlValues->GetEntityValue(Entity->GetId()))
			{
				using namespace UE::AvaPlayableRemoteControl;
				const EAvaPlayableRemoteControlResult Result = SetValueOfEntity(Entity, EntityValue->Value);
				if (Failed(Result))
				{
					UE_LOG(LogAvaPlayable, Error, TEXT("Failed to set value of exposed entity \"%s\" (id:%s): %s."),
						*Entity->GetLabel().ToString(), *Entity->GetId().ToString(), *EnumToString(Result));
				}
			}
			else
			{
				UE_LOG(LogAvaPlayable, Error, TEXT("Exposed entity \"%s\" (id:%s): value not found in page."),
					*Entity->GetLabel().ToString(), *Entity->GetId().ToString());
			}
		}
	}

	OnRemoteControlValuesApplied();

	return EAvaPlayableCommandResult::Executed;
}

void UAvaPlayable::BeginPlay(const FAvaInstancePlaySettings& InWorldPlaySettings)
{
	if (!PlayableGroup)
	{
		return;
	}

	const bool bGroupHasBegunPlay = PlayableGroup->ConditionalBeginPlay(InWorldPlaySettings);
	
	if (!bIsPlaying || bGroupHasBegunPlay)
	{
		bIsPlaying = true;
		
		// Playable events need to transit through playback events to reach the rundown for proper impl layer separation.
		UAvaSequencePlayer::OnSequenceStarted().AddUObject(this, &UAvaPlayable::HandleOnSequenceStarted);
		UAvaSequencePlayer::OnSequencePaused().AddUObject(this, &UAvaPlayable::HandleOnSequencePaused);
		UAvaSequencePlayer::OnSequenceFinished().AddUObject(this, &UAvaPlayable::HandleOnSequenceFinished);

		OnPlay();
	}
}

void UAvaPlayable::EndPlay(EAvaPlayableEndPlayOptions InOptions)
{
	if (!bIsPlaying)
	{
		return;
	}

	bIsPlaying = false;
	UAvaSequencePlayer::OnSequenceStarted().RemoveAll(this);
	UAvaSequencePlayer::OnSequencePaused().RemoveAll(this);
	UAvaSequencePlayer::OnSequenceFinished().RemoveAll(this);
	OnEndPlay();

	if (PlayableGroup && EnumHasAnyFlags(InOptions, EAvaPlayableEndPlayOptions::ConditionalEndPlayWorld) && !PlayableGroup->HasPlayingPlayables())
	{
		const bool bForceImmediate = EnumHasAnyFlags(InOptions, EAvaPlayableEndPlayOptions::ForceImmediate);
		PlayableGroup->RequestEndPlayWorld(bForceImmediate);
	}
}

bool UAvaPlayable::HasSequence(const UAvaSequence* InSequence) const
{
	const IAvaSceneInterface* SceneInterface = GetSceneInterface();
	if (!SceneInterface)
	{
		return false;
	}
	
	const IAvaSequenceProvider* SequenceProvider = SceneInterface->GetSequenceProvider();
	if (!SequenceProvider)
	{
		return false;
	}
	
	for (const TObjectPtr<UAvaSequence>& Sequence : SequenceProvider->GetSequences())
	{
		if (Sequence == InSequence)
		{
			return true;
		}
	}
	return false;
}

bool UAvaPlayable::InitPlayable(const FPlayableCreationInfo& InPlayableInfo)
{
	if (PlayableGroup)
	{
		// Register this playable in the instance group.
		// This is necessary to determine what is playing in what group.
		PlayableGroup->RegisterPlayable(this);
		return true;
	}
	
	// Currently, playables must have a playable group otherwise they are unplayable.
	UE_LOG(LogAvaPlayable, Error, TEXT("Failed to create or acquire a playable group for \"%s\". Playable will be discarded."), *InPlayableInfo.SourceAsset.ToSoftObjectPath().ToString());
	return false;
}

void UAvaPlayable::HandleOnSequenceStarted(UAvaSequencePlayer* InSequencePlayer, UAvaSequence* InSequence)
{
	if (HasSequence(InSequence))
	{
		using namespace UE::AvaPlayable::Private;
		UE_LOG(LogAvaPlayable, Verbose, TEXT("%s Playable {%s}: Sequence {%s} started."),
			*GetBriefFrameInfo(), *GetPrettyPlayableInfo(this),  *GetPrettySequenceInfo(InSequence));
		OnSequenceEventDelegate.Broadcast(this, InSequence->GetLabel(), EAvaPlayableSequenceEventType::Started);
	}
}

void UAvaPlayable::HandleOnSequencePaused(UAvaSequencePlayer* InSequencePlayer, UAvaSequence* InSequence)
{
	if (HasSequence(InSequence))
	{
		using namespace UE::AvaPlayable::Private;
		UE_LOG(LogAvaPlayable, Verbose, TEXT("%s Playable {%s}: Sequence {%s} paused."),
			*GetBriefFrameInfo(), *GetPrettyPlayableInfo(this),  *GetPrettySequenceInfo(InSequence));
		OnSequenceEventDelegate.Broadcast(this, InSequence->GetLabel(), EAvaPlayableSequenceEventType::Paused);
	}
}

void UAvaPlayable::HandleOnSequenceFinished(UAvaSequencePlayer* InSequencePlayer, UAvaSequence* InSequence)
{
	if (HasSequence(InSequence))
	{
		using namespace UE::AvaPlayable::Private;
		UE_LOG(LogAvaPlayable, Verbose, TEXT("%s Playable {%s}: Sequence {%s} finished."),
			*GetBriefFrameInfo(), *GetPrettyPlayableInfo(this),  *GetPrettySequenceInfo(InSequence));
		OnSequenceEventDelegate.Broadcast(this, InSequence->GetLabel(), EAvaPlayableSequenceEventType::Finished);
	}
}

UAvaPlayable* UAvaPlayable::CreateLocalPlayable(UObject* InOuter, const FPlayableCreationInfo& InPlayableInfo)
{
	switch (InPlayableInfo.SourceAsset.GetAssetType())
	{
	case EMotionDesignAssetType::World:
		return NewObject<UAvaPlayableLevelStreaming>(InOuter ? InOuter : GEngine);
	default:
		UE_LOG(LogAvaPlayable, Error, TEXT("Asset \"%s\" is an unsupported type."), *InPlayableInfo.SourceAsset.ToSoftObjectPath().ToString());
		return nullptr;
	}
}

UAvaPlayable* UAvaPlayable::CreateRemoteProxyPlayable(UObject* InOuter, const FPlayableCreationInfo& InPlayableInfo)
{
	return NewObject<UAvaPlayableRemoteProxy>(InOuter ? InOuter : GEngine);
}

#undef LOCTEXT_NAMESPACE
