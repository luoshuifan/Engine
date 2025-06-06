// Copyright Epic Games, Inc. All Rights Reserved.

#include "Online/OnlineServicesEOSGS.h"

#include "Online/AchievementsEOSGS.h"
#include "Online/AuthEOSGS.h"
#include "Online/LeaderboardsEOSGS.h"
#include "Online/LobbiesEOSGS.h"
#include "Online/OnlineIdEOSGS.h"
#include "Online/OnlineServicesEOSGSPlatformFactory.h"
#include "Online/StatsEOSGS.h"
#include "Online/SessionsEOSGS.h"
#include "Online/TitleFileEOSGS.h"
#include "Online/UserFileEOSGS.h"
#include "IEOSSDKManager.h"

#if WITH_ENGINE
#include "InternetAddrEOS.h"
#include "NetDriverEOSBase.h"
#include "SocketSubsystemEOS.h"
#include "SocketSubsystemEOSUtils_OnlineServicesEOSGS.h"
#endif

namespace UE::Online {

FOnlineServicesEOSGS::FOnlineServicesEOSGS(FName InInstanceName, FName InInstanceConfigName)
	: Super(GetServiceConfigNameStatic(), InInstanceName, InInstanceConfigName)
{
}

void FOnlineServicesEOSGS::Init()
{
	FOnlineServicesEOSGSPlatformFactory& PlatformFactory = FOnlineServicesEOSGSPlatformFactory::Get();
	EOSPlatformHandle = PlatformFactory.CreatePlatform(InstanceName, InstanceConfigName);
	if (EOSPlatformHandle)
	{
#if WITH_ENGINE
		SocketSubsystem = MakeShared<FSocketSubsystemEOS>(EOSPlatformHandle, MakeShared< FSocketSubsystemEOSUtils_OnlineServicesEOS>(*this));
		check(SocketSubsystem);

		FString ErrorStr;
		if (!SocketSubsystem->Init(ErrorStr))
		{
			UE_LOG(LogOnlineServices, Warning, TEXT("[FOnlineServicesEOSGS::Initialize] Unable to initialize Socket Subsystem. Error=[%s]"), *ErrorStr);
		}
#endif
	}
	else
	{
		UE_LOG(LogOnlineServices, Verbose, TEXT("[FOnlineServicesEOSGS::Initialize] Unable to initialize Socket Subsystem. EOS Platform Handle was invalid."));
		return;
	}

	Super::Init();
}

void FOnlineServicesEOSGS::Destroy()
{
	Super::Destroy();
#if WITH_ENGINE
	if (SocketSubsystem)
	{
		SocketSubsystem->Shutdown();
		SocketSubsystem = nullptr;
	}
#endif
}

void FOnlineServicesEOSGS::WarnIfEncryptionKeyMissing(const FString& InterfaceName) const
{
	if (IEOSSDKManager* Manager = IEOSSDKManager::Get())
	{
		const FString& PlatformConfigName = GetEOSPlatformHandle()->GetConfigName();
		if (const FEOSSDKPlatformConfig* Config = Manager->GetPlatformConfig(PlatformConfigName))
		{
			const FString& EncryptionKey = Config->EncryptionKey;
			if (EncryptionKey.IsEmpty())
			{
				UE_LOG(LogOnlineServices, Verbose, TEXT("%s interface not available due to missing ClientEncryptionKey in config."), *InterfaceName);
			}
			else
			{
				// If we have an encryption key and still can't get the interface, something weird is going on.
				UE_LOG(LogOnlineServices, Warning, TEXT("%s interface not available despite encryption key being present."), *InterfaceName);
			}
		}
	}
}

void FOnlineServicesEOSGS::RegisterComponents()
{
	Components.Register<FAchievementsEOSGS>(*this);
	Components.Register<FAuthEOSGS>(*this);
	Components.Register<FLeaderboardsEOSGS>(*this);
	Components.Register<FLobbiesEOSGS>(*this);
	Components.Register<FStatsEOSGS>(*this);
	Components.Register<FSessionsEOSGS>(*this);
	if (EOS_Platform_GetTitleStorageInterface(*GetEOSPlatformHandle()))
	{
		Components.Register<FTitleFileEOSGS>(*this);
	}
	else
	{
		WarnIfEncryptionKeyMissing(TEXT("TitleStorage"));
	}
	if (EOS_Platform_GetPlayerDataStorageInterface(*GetEOSPlatformHandle()))
	{
		Components.Register<FUserFileEOSGS>(*this);
	}
	else
	{
		WarnIfEncryptionKeyMissing(TEXT("PlayerDataStorage"));
	}
	Super::RegisterComponents();
}

TOnlineResult<FGetResolvedConnectString> FOnlineServicesEOSGS::GetResolvedConnectString(FGetResolvedConnectString::Params&& Params)
{
	if (Params.LobbyId.IsValid())
	{
		ILobbiesPtr LobbiesEOS = GetLobbiesInterface();
		check(LobbiesEOS);

		TOnlineResult<FGetJoinedLobbies> JoinedLobbies = LobbiesEOS->GetJoinedLobbies({ Params.LocalAccountId });
		if (JoinedLobbies.IsOk())
		{
			for (TSharedRef<const FLobby>& Lobby : JoinedLobbies.GetOkValue().Lobbies)
			{
				if (Lobby->LobbyId == Params.LobbyId)
				{
#if WITH_ENGINE
					//It should look like this: "EOS:0002aeeb5b2d4388a3752dd6d31222ec:GameNetDriver:97"
					FString NetDriverName = GetDefault<UNetDriverEOSBase>()->NetDriverName.ToString();
					FInternetAddrEOS TempAddr(GetProductUserIdChecked(Lobby->OwnerAccountId), NetDriverName, GetTypeHash(NetDriverName));
					return TOnlineResult<FGetResolvedConnectString>({ TempAddr.ToString(true) });
#else
					return TOnlineResult<FGetResolvedConnectString>(Errors::NotImplemented());
#endif
				}
			}

			// No matching lobby
			return TOnlineResult<FGetResolvedConnectString>(Errors::NotFound());
		}
		else
		{
			return TOnlineResult<FGetResolvedConnectString>(JoinedLobbies.GetErrorValue());
		}
	}
	else if (Params.SessionId.IsValid())
	{
		ISessionsPtr SessionsEOS = GetSessionsInterface();
		check(SessionsEOS);

		TOnlineResult<FGetSessionById> Result = SessionsEOS->GetSessionById({ Params.SessionId });
		if (Result.IsOk())
		{
#if WITH_ENGINE
			TSharedRef<const ISession> Session = Result.GetOkValue().Session;

			//It should look like this: "EOS:0002aeeb5b2d4388a3752dd6d31222ec:GameNetDriver:97"
			FString NetDriverName = GetDefault<UNetDriverEOSBase>()->NetDriverName.ToString();
			FInternetAddrEOS TempAddr(GetProductUserIdChecked(Session->GetOwnerAccountId()), NetDriverName, GetTypeHash(NetDriverName));
			return TOnlineResult<FGetResolvedConnectString>({ TempAddr.ToString(true) });
#else
			return TOnlineResult<FGetResolvedConnectString>(Errors::NotImplemented());
#endif
		}
		else
		{
			return TOnlineResult<FGetResolvedConnectString>(Result.GetErrorValue());
		}
	}

	// No valid lobby or session id set
	return TOnlineResult<FGetResolvedConnectString>(Errors::InvalidParams());
}

/* UE::Online */ }
