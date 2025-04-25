// Copyright Epic Games, Inc. All Rights Reserved.

#include "RTCStatsCollector.h"

#include "Logging.h"
#include "PixelStreaming2PluginSettings.h"
#include "PixelStreaming2StatNames.h"
#include "Streamer.h"
#include "UtilsString.h"

namespace UE::PixelStreaming2
{
	TSharedPtr<FRTCStatsCollector> FRTCStatsCollector::Create(const FString& PlayerId)
	{
		TSharedPtr<FRTCStatsCollector> StatsCollector = TSharedPtr<FRTCStatsCollector>(new FRTCStatsCollector(PlayerId));

		if (UPixelStreaming2PluginSettings::FDelegates* Delegates = UPixelStreaming2PluginSettings::Delegates())
		{
			Delegates->OnWebRTCDisableStatsChanged.AddSP(StatsCollector.ToSharedRef(), &FRTCStatsCollector::OnWebRTCDisableStatsChanged);
		}

		return StatsCollector;
	}

	FRTCStatsCollector::FRTCStatsCollector()
		: FRTCStatsCollector(INVALID_PLAYER_ID)
	{
	}

	FRTCStatsCollector::FRTCStatsCollector(const FString& PlayerId)
		: AssociatedPlayerId(PlayerId)
		, LastCalculationCycles(FPlatformTime::Cycles64())
		, bIsEnabled(!UPixelStreaming2PluginSettings::CVarWebRTCDisableStats.GetValueOnAnyThread())
		, CandidatePairStatsSink(MakeUnique<FCandidatePairStatsSink>(FName(*RTCStatCategories::CandidatePair)))
	{
	}

	void FRTCStatsCollector::OnWebRTCDisableStatsChanged(IConsoleVariable* Var)
	{
		bIsEnabled = !Var->GetBool();
	}

	void FRTCStatsCollector::Process(const EpicRtcConnectionStats& InStats)
	{
		FStats* PSStats = FStats::Get();
		if (!bIsEnabled || !PSStats || IsEngineExitRequested())
		{
			return;
		}

		uint64 CyclesNow = FPlatformTime::Cycles64();
		double SecondsDelta = FGenericPlatformTime::ToSeconds64(CyclesNow - LastCalculationCycles);

		// Local video stats
		for (uint64 i = 0; i < InStats._localVideoTracks._size; i++)
		{
			const EpicRtcLocalVideoTrackStats& LocalVideoTrackStats = InStats._localVideoTracks._ptr[i];

			// Process video source stats
			if (!VideoSourceSinks.Contains(i))
			{
				FName SinkName = FName(*FString::Printf(TEXT("%s [%u]"), *RTCStatCategories::VideoSource, i));
				VideoSourceSinks.Add(i, MakeUnique<FVideoSourceStatsSink>(SinkName));
			}

			VideoSourceSinks[i]->Process(LocalVideoTrackStats._source, AssociatedPlayerId, SecondsDelta);

			// Process video track rtp stats
			if (!LocalVideoTrackSinks.Contains(i))
			{
				LocalVideoTrackSinks.Add(i, {});
			}

			TMap<uint32, TUniquePtr<FRTPLocalVideoTrackStatsSink>>& SsrcSinks = LocalVideoTrackSinks[i];
			for (int j = 0; j < LocalVideoTrackStats._rtp._size; j++)
			{
				const EpicRtcLocalTrackRtpStats& RtpStats = LocalVideoTrackStats._rtp._ptr[j];

				if (!SsrcSinks.Contains(RtpStats._local._ssrc))
				{
					FName SinkName = FName(*FString::Printf(TEXT("%s [%u] (%u)"), *RTCStatCategories::LocalVideoTrack, i, RtpStats._local._ssrc));
					SsrcSinks.Add(RtpStats._local._ssrc, MakeUnique<FRTPLocalVideoTrackStatsSink>(SinkName));
				}

				SsrcSinks[RtpStats._local._ssrc]->Process(RtpStats, AssociatedPlayerId, SecondsDelta);
			}
		}

		// Local audio stats
		for (uint64 i = 0; i < InStats._localAudioTracks._size; i++)
		{
			const EpicRtcLocalAudioTrackStats& LocalAudioTrackStats = InStats._localAudioTracks._ptr[i];

			// Process audio source stats
			if (!AudioSourceSinks.Contains(i))
			{
				FName SinkName = FName(*FString::Printf(TEXT("%s [%u]"), *RTCStatCategories::AudioSource, i));
				AudioSourceSinks.Add(i, MakeUnique<FAudioSourceStatsSink>(SinkName));
			}

			AudioSourceSinks[i]->Process(LocalAudioTrackStats._source, AssociatedPlayerId, SecondsDelta);

			// Process audio track rtp stats
			if (!LocalAudioTrackSinks.Contains(i))
			{
				LocalAudioTrackSinks.Add(i, {});
			}

			TMap<uint32, TUniquePtr<FRTPLocalAudioTrackStatsSink>>& SsrcSinks = LocalAudioTrackSinks[i];
			const EpicRtcLocalTrackRtpStats&						  RtpStats = LocalAudioTrackStats._rtp;

			if (!SsrcSinks.Contains(RtpStats._local._ssrc))
			{
				FName SinkName = FName(*FString::Printf(TEXT("%s [%u] (%u)"), *RTCStatCategories::LocalAudioTrack, i, RtpStats._local._ssrc));
				SsrcSinks.Add(RtpStats._local._ssrc, MakeUnique<FRTPLocalAudioTrackStatsSink>(SinkName));
			}

			SsrcSinks[RtpStats._local._ssrc]->Process(RtpStats, AssociatedPlayerId, SecondsDelta);
		}

		// remote video stats
		for (uint64 i = 0; i < InStats._remoteVideoTracks._size; i++)
		{
			const EpicRtcRemoteTrackStats& RemoteVideoTrackStats = InStats._remoteVideoTracks._ptr[i];

			// Process video track rtp stats
			if (!RemoteVideoTrackSinks.Contains(i))
			{
				RemoteVideoTrackSinks.Add(i, {});
			}

			TMap<uint32, TUniquePtr<FRTPRemoteTrackStatsSink>>& SsrcSinks = RemoteVideoTrackSinks[i];
			const EpicRtcRemoteTrackRtpStats&					  RtpStats = RemoteVideoTrackStats._rtp;

			if (!SsrcSinks.Contains(RtpStats._local._ssrc))
			{
				FName SinkName = FName(*FString::Printf(TEXT("%s [%u] (%u)"), *RTCStatCategories::RemoteVideoTrack, i, RtpStats._local._ssrc));
				SsrcSinks.Add(RtpStats._local._ssrc, MakeUnique<FRTPRemoteTrackStatsSink>(SinkName));
			}

			SsrcSinks[RtpStats._local._ssrc]->Process(RtpStats, AssociatedPlayerId, SecondsDelta);
		}

		// remote audio stats
		for (uint64 i = 0; i < InStats._remoteAudioTracks._size; i++)
		{
			const EpicRtcRemoteTrackStats& RemoteAudioTrackStats = InStats._remoteAudioTracks._ptr[i];

			// Process audio track rtp stats
			if (!RemoteAudioTrackSinks.Contains(i))
			{
				RemoteAudioTrackSinks.Add(i, {});
			}

			TMap<uint32, TUniquePtr<FRTPRemoteTrackStatsSink>>& SsrcSinks = RemoteVideoTrackSinks[i];
			const EpicRtcRemoteTrackRtpStats&					  RtpStats = RemoteAudioTrackStats._rtp;

			if (!SsrcSinks.Contains(RtpStats._local._ssrc))
			{
				FName SinkName = FName(*FString::Printf(TEXT("%s [%u] (%u)"), *RTCStatCategories::RemoteAudioTrack, i, RtpStats._local._ssrc));
				SsrcSinks.Add(RtpStats._local._ssrc, MakeUnique<FRTPRemoteTrackStatsSink>(SinkName));
			}

			SsrcSinks[RtpStats._local._ssrc]->Process(RtpStats, AssociatedPlayerId, SecondsDelta);
		}

		// data track stats
		for (uint64 i = 0; i < InStats._dataTracks._size; i++)
		{
			const EpicRtcDataTrackStats& DataTrackStats = InStats._dataTracks._ptr[i];

			// Process data track stats
			if (!DataTrackSinks.Contains(i))
			{
				FName SinkName = FName(*FString::Printf(TEXT("%s [%u]"), *RTCStatCategories::DataChannel, i));
				DataTrackSinks.Add(i, MakeUnique<FDataTrackStatsSink>(SinkName));
			}

			DataTrackSinks[i]->Process(DataTrackStats, AssociatedPlayerId, SecondsDelta);
		}

		// transport stats
		if (InStats._transports._size > 0)
		{
			//(Nazar.Rudenko): More than one transport is possible only if we are not using bundle which we do
			const EpicRtcTransportStats& Transport = InStats._transports._ptr[0];
			FString						 SelectedPairId = ToString(Transport._selectedCandidatePairId);
			for (int i = 0; i < Transport._candidatePairs._size; i++)
			{
				FString PairId = ToString(Transport._candidatePairs._ptr[i]._id);
				if (SelectedPairId == PairId)
				{
					CandidatePairStatsSink->Process(Transport._candidatePairs._ptr[i], AssociatedPlayerId, SecondsDelta);
				}
			}
		}

		LastCalculationCycles = CyclesNow;
	}
	/**
	 * ---------- FRTPLocalVideoTrackSink ----------
	 */
	FRTCStatsCollector::FRTPLocalVideoTrackStatsSink::FRTPLocalVideoTrackStatsSink(FName InCategory)
		: FStatsSink(InCategory)
	{
		// These stats will be extracted from the stat reports and emitted straight to screen
		Add(PixelStreaming2StatNames::FirCount, 0);
		Add(PixelStreaming2StatNames::PliCount, 0);
		Add(PixelStreaming2StatNames::NackCount, 0);
		Add(PixelStreaming2StatNames::RetransmittedBytesSent, 0);
		Add(PixelStreaming2StatNames::TotalEncodeBytesTarget, 0);
		Add(PixelStreaming2StatNames::KeyFramesEncoded, 0);
		Add(PixelStreaming2StatNames::FrameWidth, 0);
		Add(PixelStreaming2StatNames::FrameHeight, 0);
		Add(PixelStreaming2StatNames::HugeFramesSent, 0);
		Add(PixelStreaming2StatNames::PacketsLost, 0);
		Add(PixelStreaming2StatNames::Jitter, 0);
		Add(PixelStreaming2StatNames::RoundTripTime, 0);

		// These are values used to calculate extra values (stores time deltas etc)
		AddNonRendered(PixelStreaming2StatNames::TargetBitrate);
		AddNonRendered(PixelStreaming2StatNames::FramesSent);
		AddNonRendered(PixelStreaming2StatNames::FramesReceived);
		AddNonRendered(PixelStreaming2StatNames::BytesSent);
		AddNonRendered(PixelStreaming2StatNames::BytesReceived);
		AddNonRendered(PixelStreaming2StatNames::QPSum);
		AddNonRendered(PixelStreaming2StatNames::TotalEncodeTime);
		AddNonRendered(PixelStreaming2StatNames::FramesEncoded);
		AddNonRendered(PixelStreaming2StatNames::FramesDecoded);
		AddNonRendered(PixelStreaming2StatNames::TotalPacketSendDelay);

		// Calculated stats below:
		// FrameSent Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* FramesSentStat = StatSource.Get(PixelStreaming2StatNames::FramesSent);
			if (FramesSentStat && FramesSentStat->GetLatestStat().StatValue > 0)
			{
				const double FramesSentPerSecond = FramesSentStat->CalculateDelta(Period);
				FStatData	 FpsStat = FStatData(PixelStreaming2StatNames::FramesSentPerSecond, FramesSentPerSecond, 0);
				FpsStat.DisplayFlags = FStatData::EDisplayFlags::TEXT | FStatData::EDisplayFlags::GRAPH;
				return FpsStat;
			}
			return {};
		});

		// FramesReceived Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* FramesReceivedStat = StatSource.Get(PixelStreaming2StatNames::FramesReceived);
			if (FramesReceivedStat && FramesReceivedStat->GetLatestStat().StatValue > 0)
			{
				const double FramesReceivedPerSecond = FramesReceivedStat->CalculateDelta(Period);
				return FStatData(PixelStreaming2StatNames::FramesReceivedPerSecond, FramesReceivedPerSecond, 0);
			}
			return {};
		});

		// Megabits sent Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* BytesSentStat = StatSource.Get(PixelStreaming2StatNames::BytesSent);
			if (BytesSentStat && BytesSentStat->GetLatestStat().StatValue > 0)
			{
				const double BytesSentPerSecond = BytesSentStat->CalculateDelta(Period);
				const double MegabitsPerSecond = BytesSentPerSecond / 1'000'000.0 * 8.0;
				return FStatData(PixelStreaming2StatNames::BitrateMegabits, MegabitsPerSecond, 2);
			}
			return {};
		});

		// Bits sent Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* BytesSentStat = StatSource.Get(PixelStreaming2StatNames::BytesSent);
			if (BytesSentStat && BytesSentStat->GetLatestStat().StatValue > 0)
			{
				const double BytesSentPerSecond = BytesSentStat->CalculateDelta(Period);
				const double BitsPerSecond = BytesSentPerSecond * 8.0;
				FStatData	 Stat = FStatData(PixelStreaming2StatNames::Bitrate, BitsPerSecond, 0);
				Stat.DisplayFlags = FStatData::EDisplayFlags::HIDDEN; // We don't want to display bits per second (too many digits)
				return Stat;
			}
			return {};
		});

		// Target megabits sent Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* TargetBpsStats = StatSource.Get(PixelStreaming2StatNames::TargetBitrate);
			if (TargetBpsStats && TargetBpsStats->GetLatestStat().StatValue > 0)
			{
				const double TargetBps = TargetBpsStats->Average();
				const double MegabitsPerSecond = TargetBps / 1'000'000.0;
				return FStatData(PixelStreaming2StatNames::TargetBitrateMegabits, MegabitsPerSecond, 2);
			}
			return {};
		});

		// Megabits received Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* BytesReceivedStat = StatSource.Get(PixelStreaming2StatNames::BytesReceived);
			if (BytesReceivedStat && BytesReceivedStat->GetLatestStat().StatValue > 0)
			{
				const double BytesReceivedPerSecond = BytesReceivedStat->CalculateDelta(Period);
				const double MegabitsPerSecond = BytesReceivedPerSecond / 1000.0 * 8.0;
				return FStatData(PixelStreaming2StatNames::Bitrate, MegabitsPerSecond, 2);
			}
			return {};
		});

		// Encoded fps
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* EncodedFramesStat = StatSource.Get(PixelStreaming2StatNames::FramesEncoded);
			if (EncodedFramesStat && EncodedFramesStat->GetLatestStat().StatValue > 0)
			{
				const double EncodedFramesPerSecond = EncodedFramesStat->CalculateDelta(Period);
				return FStatData(PixelStreaming2StatNames::EncodedFramesPerSecond, EncodedFramesPerSecond, 0);
			}
			return {};
		});

		// Decoded fps
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* DecodedFramesStat = StatSource.Get(PixelStreaming2StatNames::FramesDecoded);
			if (DecodedFramesStat && DecodedFramesStat->GetLatestStat().StatValue > 0)
			{
				const double DecodedFramesPerSecond = DecodedFramesStat->CalculateDelta(Period);
				return FStatData(PixelStreaming2StatNames::DecodedFramesPerSecond, DecodedFramesPerSecond, 0);
			}
			return {};
		});

		// Avg QP Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* QPSumStat = StatSource.Get(PixelStreaming2StatNames::QPSum);
			FStatData*		 EncodedFramesPerSecond = StatSource.GetCalculatedStat(PixelStreaming2StatNames::EncodedFramesPerSecond);
			if (QPSumStat && QPSumStat->GetLatestStat().StatValue > 0
				&& EncodedFramesPerSecond && EncodedFramesPerSecond->StatValue > 0.0)
			{
				const double QPSumDeltaPerSecond = QPSumStat->CalculateDelta(Period);
				const double MeanQPPerFrame = QPSumDeltaPerSecond / EncodedFramesPerSecond->StatValue;
				FName		 StatName = PixelStreaming2StatNames::MeanQPPerSecond;
				return FStatData(StatName, MeanQPPerFrame, 0);
			}
			return {};
		});

		// Mean EncodeTime (ms) Per Frame
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* TotalEncodeTimeStat = StatSource.Get(PixelStreaming2StatNames::TotalEncodeTime);
			FStatData*		 EncodedFramesPerSecond = StatSource.GetCalculatedStat(PixelStreaming2StatNames::EncodedFramesPerSecond);
			if (TotalEncodeTimeStat && TotalEncodeTimeStat->GetLatestStat().StatValue > 0
				&& EncodedFramesPerSecond && EncodedFramesPerSecond->StatValue > 0.0)
			{
				const double TotalEncodeTimePerSecond = TotalEncodeTimeStat->CalculateDelta(Period);
				const double MeanEncodeTimePerFrameMs = TotalEncodeTimePerSecond / EncodedFramesPerSecond->StatValue * 1000.0;
				return FStatData(PixelStreaming2StatNames::MeanEncodeTime, MeanEncodeTimePerFrameMs, 2);
			}
			return {};
		});

		// Mean SendDelay (ms) Per Frame
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* TotalSendDelayStat = StatSource.Get(PixelStreaming2StatNames::TotalPacketSendDelay);
			FStatData*		 FramesSentPerSecond = StatSource.GetCalculatedStat(PixelStreaming2StatNames::FramesSentPerSecond);
			if (TotalSendDelayStat && TotalSendDelayStat->GetLatestStat().StatValue > 0
				&& FramesSentPerSecond && FramesSentPerSecond->StatValue > 0.0)
			{
				const double TotalSendDelayPerSecond = TotalSendDelayStat->CalculateDelta(Period);
				const double MeanSendDelayPerFrameMs = TotalSendDelayPerSecond / FramesSentPerSecond->StatValue * 1000.0;
				return FStatData(PixelStreaming2StatNames::MeanSendDelay, MeanSendDelayPerFrameMs, 2);
			}
			return {};
		});

		// JitterBufferDelay (ms)
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* JitterBufferDelayStat = StatSource.Get(PixelStreaming2StatNames::JitterBufferDelay);
			FStatData*		 FramesReceivedPerSecond = StatSource.GetCalculatedStat(PixelStreaming2StatNames::FramesReceivedPerSecond);
			if (JitterBufferDelayStat && JitterBufferDelayStat->GetLatestStat().StatValue > 0
				&& FramesReceivedPerSecond && FramesReceivedPerSecond->StatValue > 0.0)
			{
				const double TotalJitterBufferDelayPerSecond = JitterBufferDelayStat->CalculateDelta(Period);
				const double MeanJitterBufferDelayMs = TotalJitterBufferDelayPerSecond / FramesReceivedPerSecond->StatValue * 1000.0;
				return FStatData(PixelStreaming2StatNames::JitterBufferDelay, MeanJitterBufferDelayMs, 2);
			}
			return {};
		});
	}

	void FRTCStatsCollector::FRTPLocalVideoTrackStatsSink::Process(const EpicRtcLocalTrackRtpStats& InStats, const FString& PeerId, double SecondsDelta)
	{
		FStats* PSStats = FStats::Get();
		if (!PSStats)
		{
			return;
		}

		for (TPair<FName, FRTCTrackedStat>& Tuple : Stats)
		{
			double NewValue = 0;
			if (Tuple.Key == PixelStreaming2StatNames::FirCount)
			{
				NewValue = InStats._local._firCount;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::PliCount)
			{
				NewValue = InStats._local._pliCount;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::NackCount)
			{
				NewValue = InStats._local._nackCount;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::RetransmittedBytesSent)
			{
				NewValue = InStats._local._retransmittedBytesSent;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::TotalEncodeBytesTarget)
			{
				NewValue = InStats._local._totalEncodedBytesTarget;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::KeyFramesEncoded)
			{
				NewValue = InStats._local._keyFramesEncoded;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::FrameWidth)
			{
				NewValue = InStats._local._frameWidth;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::FrameHeight)
			{
				NewValue = InStats._local._frameHeight;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::HugeFramesSent)
			{
				NewValue = InStats._local._hugeFramesSent;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::TotalPacketSendDelay)
			{
				NewValue = InStats._local._totalPacketSendDelay;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::TargetBitrate)
			{
				NewValue = InStats._local._targetBitrate;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::FramesSent)
			{
				NewValue = InStats._local._framesSent;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::FramesReceived)
			{
				// TODO(Nazar.Rudenko): Available for inbound tracks only
			}
			else if (Tuple.Key == PixelStreaming2StatNames::BytesSent)
			{
				NewValue = InStats._local._bytesSent;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::BytesReceived)
			{
				// TODO(Nazar.Rudenko): Available for inbound tracks only
			}
			else if (Tuple.Key == PixelStreaming2StatNames::QPSum)
			{
				NewValue = InStats._local._qpSum;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::TotalEncodeTime)
			{
				NewValue = InStats._local._totalEncodeTime;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::FramesEncoded)
			{
				NewValue = InStats._local._framesEncoded;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::FramesDecoded)
			{
				// TODO(Nazar.Rudenko): Available for inbound tracks only
			}
			else if (Tuple.Key == PixelStreaming2StatNames::PacketsLost)
			{
				NewValue = InStats._remote._packetsLost;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::Jitter)
			{
				NewValue = InStats._remote._jitter;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::RoundTripTime)
			{
				NewValue = InStats._remote._roundTripTime;
			}

			if (UpdateValue(NewValue, &Tuple.Value))
			{
				PSStats->StorePeerStat(PeerId, Category, Tuple.Value.GetLatestStat());
			}
		}

		PostProcess(PSStats, PeerId, SecondsDelta);
	}

	/**
	 * ---------- FRTPLocalAudioTrackStatsSink ----------
	 */
	FRTCStatsCollector::FRTPLocalAudioTrackStatsSink::FRTPLocalAudioTrackStatsSink(FName InCategory)
		: FStatsSink(InCategory)
	{
		// These stats will be extracted from the stat reports and emitted straight to screen
		Add(PixelStreaming2StatNames::FirCount, 0);
		Add(PixelStreaming2StatNames::PliCount, 0);
		Add(PixelStreaming2StatNames::NackCount, 0);
		Add(PixelStreaming2StatNames::RetransmittedBytesSent, 0);
		Add(PixelStreaming2StatNames::TotalEncodeBytesTarget, 0);
		Add(PixelStreaming2StatNames::KeyFramesEncoded, 0);
		Add(PixelStreaming2StatNames::FrameWidth, 0);
		Add(PixelStreaming2StatNames::FrameHeight, 0);
		Add(PixelStreaming2StatNames::HugeFramesSent, 0);
		Add(PixelStreaming2StatNames::PacketsLost, 0);
		Add(PixelStreaming2StatNames::Jitter, 0);
		Add(PixelStreaming2StatNames::RoundTripTime, 0);

		// These are values used to calculate extra values (stores time deltas etc)
		AddNonRendered(PixelStreaming2StatNames::TargetBitrate);
		AddNonRendered(PixelStreaming2StatNames::FramesSent);
		AddNonRendered(PixelStreaming2StatNames::FramesReceived);
		AddNonRendered(PixelStreaming2StatNames::BytesSent);
		AddNonRendered(PixelStreaming2StatNames::BytesReceived);
		AddNonRendered(PixelStreaming2StatNames::QPSum);
		AddNonRendered(PixelStreaming2StatNames::TotalEncodeTime);
		AddNonRendered(PixelStreaming2StatNames::FramesEncoded);
		AddNonRendered(PixelStreaming2StatNames::FramesDecoded);
		AddNonRendered(PixelStreaming2StatNames::TotalPacketSendDelay);

		// Calculated stats below:
		// FrameSent Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* FramesSentStat = StatSource.Get(PixelStreaming2StatNames::FramesSent);
			if (FramesSentStat && FramesSentStat->GetLatestStat().StatValue > 0)
			{
				const double FramesSentPerSecond = FramesSentStat->CalculateDelta(Period);
				FStatData	 FpsStat = FStatData(PixelStreaming2StatNames::FramesSentPerSecond, FramesSentPerSecond, 0);
				FpsStat.DisplayFlags = FStatData::EDisplayFlags::TEXT | FStatData::EDisplayFlags::GRAPH;
				return FpsStat;
			}
			return {};
		});

		// FramesReceived Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* FramesReceivedStat = StatSource.Get(PixelStreaming2StatNames::FramesReceived);
			if (FramesReceivedStat && FramesReceivedStat->GetLatestStat().StatValue > 0)
			{
				const double FramesReceivedPerSecond = FramesReceivedStat->CalculateDelta(Period);
				return FStatData(PixelStreaming2StatNames::FramesReceivedPerSecond, FramesReceivedPerSecond, 0);
			}
			return {};
		});

		// Megabits sent Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* BytesSentStat = StatSource.Get(PixelStreaming2StatNames::BytesSent);
			if (BytesSentStat && BytesSentStat->GetLatestStat().StatValue > 0)
			{
				const double BytesSentPerSecond = BytesSentStat->CalculateDelta(Period);
				const double MegabitsPerSecond = BytesSentPerSecond / 1'000'000.0 * 8.0;
				return FStatData(PixelStreaming2StatNames::BitrateMegabits, MegabitsPerSecond, 2);
			}
			return {};
		});

		// Bits sent Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* BytesSentStat = StatSource.Get(PixelStreaming2StatNames::BytesSent);
			if (BytesSentStat && BytesSentStat->GetLatestStat().StatValue > 0)
			{
				const double BytesSentPerSecond = BytesSentStat->CalculateDelta(Period);
				const double BitsPerSecond = BytesSentPerSecond * 8.0;
				FStatData	 Stat = FStatData(PixelStreaming2StatNames::Bitrate, BitsPerSecond, 0);
				Stat.DisplayFlags = FStatData::EDisplayFlags::HIDDEN; // We don't want to display bits per second (too many digits)
				return Stat;
			}
			return {};
		});

		// Target megabits sent Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* TargetBpsStats = StatSource.Get(PixelStreaming2StatNames::TargetBitrate);
			if (TargetBpsStats && TargetBpsStats->GetLatestStat().StatValue > 0)
			{
				const double TargetBps = TargetBpsStats->Average();
				const double MegabitsPerSecond = TargetBps / 1'000'000.0;
				return FStatData(PixelStreaming2StatNames::TargetBitrateMegabits, MegabitsPerSecond, 2);
			}
			return {};
		});

		// Megabits received Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* BytesReceivedStat = StatSource.Get(PixelStreaming2StatNames::BytesReceived);
			if (BytesReceivedStat && BytesReceivedStat->GetLatestStat().StatValue > 0)
			{
				const double BytesReceivedPerSecond = BytesReceivedStat->CalculateDelta(Period);
				const double MegabitsPerSecond = BytesReceivedPerSecond / 1000.0 * 8.0;
				return FStatData(PixelStreaming2StatNames::Bitrate, MegabitsPerSecond, 2);
			}
			return {};
		});

		// Encoded fps
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* EncodedFramesStat = StatSource.Get(PixelStreaming2StatNames::FramesEncoded);
			if (EncodedFramesStat && EncodedFramesStat->GetLatestStat().StatValue > 0)
			{
				const double EncodedFramesPerSecond = EncodedFramesStat->CalculateDelta(Period);
				return FStatData(PixelStreaming2StatNames::EncodedFramesPerSecond, EncodedFramesPerSecond, 0);
			}
			return {};
		});

		// Decoded fps
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* DecodedFramesStat = StatSource.Get(PixelStreaming2StatNames::FramesDecoded);
			if (DecodedFramesStat && DecodedFramesStat->GetLatestStat().StatValue > 0)
			{
				const double DecodedFramesPerSecond = DecodedFramesStat->CalculateDelta(Period);
				return FStatData(PixelStreaming2StatNames::DecodedFramesPerSecond, DecodedFramesPerSecond, 0);
			}
			return {};
		});

		// Avg QP Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* QPSumStat = StatSource.Get(PixelStreaming2StatNames::QPSum);
			FStatData*		 EncodedFramesPerSecond = StatSource.GetCalculatedStat(PixelStreaming2StatNames::EncodedFramesPerSecond);
			if (QPSumStat && QPSumStat->GetLatestStat().StatValue > 0
				&& EncodedFramesPerSecond && EncodedFramesPerSecond->StatValue > 0.0)
			{
				const double QPSumDeltaPerSecond = QPSumStat->CalculateDelta(Period);
				const double MeanQPPerFrame = QPSumDeltaPerSecond / EncodedFramesPerSecond->StatValue;
				FName		 StatName = PixelStreaming2StatNames::MeanQPPerSecond;
				return FStatData(StatName, MeanQPPerFrame, 0);
			}
			return {};
		});

		// Mean EncodeTime (ms) Per Frame
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* TotalEncodeTimeStat = StatSource.Get(PixelStreaming2StatNames::TotalEncodeTime);
			FStatData*		 EncodedFramesPerSecond = StatSource.GetCalculatedStat(PixelStreaming2StatNames::EncodedFramesPerSecond);
			if (TotalEncodeTimeStat && TotalEncodeTimeStat->GetLatestStat().StatValue > 0
				&& EncodedFramesPerSecond && EncodedFramesPerSecond->StatValue > 0.0)
			{
				const double TotalEncodeTimePerSecond = TotalEncodeTimeStat->CalculateDelta(Period);
				const double MeanEncodeTimePerFrameMs = TotalEncodeTimePerSecond / EncodedFramesPerSecond->StatValue * 1000.0;
				return FStatData(PixelStreaming2StatNames::MeanEncodeTime, MeanEncodeTimePerFrameMs, 2);
			}
			return {};
		});

		// Mean SendDelay (ms) Per Frame
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* TotalSendDelayStat = StatSource.Get(PixelStreaming2StatNames::TotalPacketSendDelay);
			FStatData*		 FramesSentPerSecond = StatSource.GetCalculatedStat(PixelStreaming2StatNames::FramesSentPerSecond);
			if (TotalSendDelayStat && TotalSendDelayStat->GetLatestStat().StatValue > 0
				&& FramesSentPerSecond && FramesSentPerSecond->StatValue > 0.0)
			{
				const double TotalSendDelayPerSecond = TotalSendDelayStat->CalculateDelta(Period);
				const double MeanSendDelayPerFrameMs = TotalSendDelayPerSecond / FramesSentPerSecond->StatValue * 1000.0;
				return FStatData(PixelStreaming2StatNames::MeanSendDelay, MeanSendDelayPerFrameMs, 2);
			}
			return {};
		});

		// JitterBufferDelay (ms)
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* JitterBufferDelayStat = StatSource.Get(PixelStreaming2StatNames::JitterBufferDelay);
			FStatData*		 FramesReceivedPerSecond = StatSource.GetCalculatedStat(PixelStreaming2StatNames::FramesReceivedPerSecond);
			if (JitterBufferDelayStat && JitterBufferDelayStat->GetLatestStat().StatValue > 0
				&& FramesReceivedPerSecond && FramesReceivedPerSecond->StatValue > 0.0)
			{
				const double TotalJitterBufferDelayPerSecond = JitterBufferDelayStat->CalculateDelta(Period);
				const double MeanJitterBufferDelayMs = TotalJitterBufferDelayPerSecond / FramesReceivedPerSecond->StatValue * 1000.0;
				return FStatData(PixelStreaming2StatNames::JitterBufferDelay, MeanJitterBufferDelayMs, 2);
			}
			return {};
		});
	}

	void FRTCStatsCollector::FRTPLocalAudioTrackStatsSink::Process(const EpicRtcLocalTrackRtpStats& InStats, const FString& PeerId, double SecondsDelta)
	{
		FStats* PSStats = FStats::Get();
		if (!PSStats)
		{
			return;
		}

		for (TPair<FName, FRTCTrackedStat>& Tuple : Stats)
		{
			double NewValue = 0;
			if (Tuple.Key == PixelStreaming2StatNames::TotalPacketSendDelay)
			{
				NewValue = InStats._local._totalPacketSendDelay;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::TargetBitrate)
			{
				NewValue = InStats._local._targetBitrate;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::BytesSent)
			{
				NewValue = InStats._local._bytesSent;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::PacketsLost)
			{
				NewValue = InStats._remote._packetsLost;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::Jitter)
			{
				NewValue = InStats._remote._jitter;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::RoundTripTime)
			{
				NewValue = InStats._remote._roundTripTime;
			}

			if (UpdateValue(NewValue, &Tuple.Value))
			{
				PSStats->StorePeerStat(PeerId, Category, Tuple.Value.GetLatestStat());
			}
		}

		PostProcess(PSStats, PeerId, SecondsDelta);
	}

	/**
	 * ---------- FRTPRemoteTrackStatsSink ----------
	 */
	FRTCStatsCollector::FRTPRemoteTrackStatsSink::FRTPRemoteTrackStatsSink(FName InCategory)
		: FStatsSink(InCategory)
	{
		// These stats will be extracted from the stat reports and emitted straight to screen
		Add(PixelStreaming2StatNames::FirCount, 0);
		Add(PixelStreaming2StatNames::PliCount, 0);
		Add(PixelStreaming2StatNames::NackCount, 0);
		Add(PixelStreaming2StatNames::RetransmittedBytesReceived, 0);
		Add(PixelStreaming2StatNames::RetransmittedPacketsReceived, 0);
		Add(PixelStreaming2StatNames::TotalEncodeBytesTarget, 0);
		Add(PixelStreaming2StatNames::KeyFramesDecoded, 0);
		Add(PixelStreaming2StatNames::FrameWidth, 0);
		Add(PixelStreaming2StatNames::FrameHeight, 0);
		Add(PixelStreaming2StatNames::HugeFramesSent, 0);
		Add(PixelStreaming2StatNames::PacketsLost, 0);
		Add(PixelStreaming2StatNames::Jitter, 0);
		Add(PixelStreaming2StatNames::RoundTripTime, 0);

		// These are values used to calculate extra values (stores time deltas etc)
		AddNonRendered(PixelStreaming2StatNames::TargetBitrate);
		AddNonRendered(PixelStreaming2StatNames::FramesSent);
		AddNonRendered(PixelStreaming2StatNames::FramesReceived);
		AddNonRendered(PixelStreaming2StatNames::BytesSent);
		AddNonRendered(PixelStreaming2StatNames::BytesReceived);
		AddNonRendered(PixelStreaming2StatNames::QPSum);
		AddNonRendered(PixelStreaming2StatNames::TotalEncodeTime);
		AddNonRendered(PixelStreaming2StatNames::FramesEncoded);
		AddNonRendered(PixelStreaming2StatNames::FramesDecoded);
		AddNonRendered(PixelStreaming2StatNames::TotalPacketSendDelay);

		// Calculated stats below:
		// FrameSent Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* FramesSentStat = StatSource.Get(PixelStreaming2StatNames::FramesSent);
			if (FramesSentStat && FramesSentStat->GetLatestStat().StatValue > 0)
			{
				const double FramesSentPerSecond = FramesSentStat->CalculateDelta(Period);
				FStatData	 FpsStat = FStatData(PixelStreaming2StatNames::FramesSentPerSecond, FramesSentPerSecond, 0);
				FpsStat.DisplayFlags = FStatData::EDisplayFlags::TEXT | FStatData::EDisplayFlags::GRAPH;
				return FpsStat;
			}
			return {};
		});

		// FramesReceived Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* FramesReceivedStat = StatSource.Get(PixelStreaming2StatNames::FramesReceived);
			if (FramesReceivedStat && FramesReceivedStat->GetLatestStat().StatValue > 0)
			{
				const double FramesReceivedPerSecond = FramesReceivedStat->CalculateDelta(Period);
				return FStatData(PixelStreaming2StatNames::FramesReceivedPerSecond, FramesReceivedPerSecond, 0);
			}
			return {};
		});

		// Megabits sent Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* BytesSentStat = StatSource.Get(PixelStreaming2StatNames::BytesSent);
			if (BytesSentStat && BytesSentStat->GetLatestStat().StatValue > 0)
			{
				const double BytesSentPerSecond = BytesSentStat->CalculateDelta(Period);
				const double MegabitsPerSecond = BytesSentPerSecond / 1'000'000.0 * 8.0;
				return FStatData(PixelStreaming2StatNames::BitrateMegabits, MegabitsPerSecond, 2);
			}
			return {};
		});

		// Bits sent Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* BytesSentStat = StatSource.Get(PixelStreaming2StatNames::BytesSent);
			if (BytesSentStat && BytesSentStat->GetLatestStat().StatValue > 0)
			{
				const double BytesSentPerSecond = BytesSentStat->CalculateDelta(Period);
				const double BitsPerSecond = BytesSentPerSecond * 8.0;
				FStatData	 Stat = FStatData(PixelStreaming2StatNames::Bitrate, BitsPerSecond, 0);
				Stat.DisplayFlags = FStatData::EDisplayFlags::HIDDEN; // We don't want to display bits per second (too many digits)
				return Stat;
			}
			return {};
		});

		// Target megabits sent Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* TargetBpsStats = StatSource.Get(PixelStreaming2StatNames::TargetBitrate);
			if (TargetBpsStats && TargetBpsStats->GetLatestStat().StatValue > 0)
			{
				const double TargetBps = TargetBpsStats->Average();
				const double MegabitsPerSecond = TargetBps / 1'000'000.0;
				return FStatData(PixelStreaming2StatNames::TargetBitrateMegabits, MegabitsPerSecond, 2);
			}
			return {};
		});

		// Megabits received Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* BytesReceivedStat = StatSource.Get(PixelStreaming2StatNames::BytesReceived);
			if (BytesReceivedStat && BytesReceivedStat->GetLatestStat().StatValue > 0)
			{
				const double BytesReceivedPerSecond = BytesReceivedStat->CalculateDelta(Period);
				const double MegabitsPerSecond = BytesReceivedPerSecond / 1000.0 * 8.0;
				return FStatData(PixelStreaming2StatNames::Bitrate, MegabitsPerSecond, 2);
			}
			return {};
		});

		// Encoded fps
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* EncodedFramesStat = StatSource.Get(PixelStreaming2StatNames::FramesEncoded);
			if (EncodedFramesStat && EncodedFramesStat->GetLatestStat().StatValue > 0)
			{
				const double EncodedFramesPerSecond = EncodedFramesStat->CalculateDelta(Period);
				return FStatData(PixelStreaming2StatNames::EncodedFramesPerSecond, EncodedFramesPerSecond, 0);
			}
			return {};
		});

		// Decoded fps
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* DecodedFramesStat = StatSource.Get(PixelStreaming2StatNames::FramesDecoded);
			if (DecodedFramesStat && DecodedFramesStat->GetLatestStat().StatValue > 0)
			{
				const double DecodedFramesPerSecond = DecodedFramesStat->CalculateDelta(Period);
				return FStatData(PixelStreaming2StatNames::DecodedFramesPerSecond, DecodedFramesPerSecond, 0);
			}
			return {};
		});

		// Avg QP Per Second
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* QPSumStat = StatSource.Get(PixelStreaming2StatNames::QPSum);
			FStatData*		 EncodedFramesPerSecond = StatSource.GetCalculatedStat(PixelStreaming2StatNames::EncodedFramesPerSecond);
			if (QPSumStat && QPSumStat->GetLatestStat().StatValue > 0
				&& EncodedFramesPerSecond && EncodedFramesPerSecond->StatValue > 0.0)
			{
				const double QPSumDeltaPerSecond = QPSumStat->CalculateDelta(Period);
				const double MeanQPPerFrame = QPSumDeltaPerSecond / EncodedFramesPerSecond->StatValue;
				FName		 StatName = PixelStreaming2StatNames::MeanQPPerSecond;
				return FStatData(StatName, MeanQPPerFrame, 0);
			}
			return {};
		});

		// Mean EncodeTime (ms) Per Frame
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* TotalEncodeTimeStat = StatSource.Get(PixelStreaming2StatNames::TotalEncodeTime);
			FStatData*		 EncodedFramesPerSecond = StatSource.GetCalculatedStat(PixelStreaming2StatNames::EncodedFramesPerSecond);
			if (TotalEncodeTimeStat && TotalEncodeTimeStat->GetLatestStat().StatValue > 0
				&& EncodedFramesPerSecond && EncodedFramesPerSecond->StatValue > 0.0)
			{
				const double TotalEncodeTimePerSecond = TotalEncodeTimeStat->CalculateDelta(Period);
				const double MeanEncodeTimePerFrameMs = TotalEncodeTimePerSecond / EncodedFramesPerSecond->StatValue * 1000.0;
				return FStatData(PixelStreaming2StatNames::MeanEncodeTime, MeanEncodeTimePerFrameMs, 2);
			}
			return {};
		});

		// Mean SendDelay (ms) Per Frame
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* TotalSendDelayStat = StatSource.Get(PixelStreaming2StatNames::TotalPacketSendDelay);
			FStatData*		 FramesSentPerSecond = StatSource.GetCalculatedStat(PixelStreaming2StatNames::FramesSentPerSecond);
			if (TotalSendDelayStat && TotalSendDelayStat->GetLatestStat().StatValue > 0
				&& FramesSentPerSecond && FramesSentPerSecond->StatValue > 0.0)
			{
				const double TotalSendDelayPerSecond = TotalSendDelayStat->CalculateDelta(Period);
				const double MeanSendDelayPerFrameMs = TotalSendDelayPerSecond / FramesSentPerSecond->StatValue * 1000.0;
				return FStatData(PixelStreaming2StatNames::MeanSendDelay, MeanSendDelayPerFrameMs, 2);
			}
			return {};
		});

		// JitterBufferDelay (ms)
		AddStatCalculator([](FStatsSink& StatSource, double Period) -> TOptional<FStatData> {
			FRTCTrackedStat* JitterBufferDelayStat = StatSource.Get(PixelStreaming2StatNames::JitterBufferDelay);
			FStatData*		 FramesReceivedPerSecond = StatSource.GetCalculatedStat(PixelStreaming2StatNames::FramesReceivedPerSecond);
			if (JitterBufferDelayStat && JitterBufferDelayStat->GetLatestStat().StatValue > 0
				&& FramesReceivedPerSecond && FramesReceivedPerSecond->StatValue > 0.0)
			{
				const double TotalJitterBufferDelayPerSecond = JitterBufferDelayStat->CalculateDelta(Period);
				const double MeanJitterBufferDelayMs = TotalJitterBufferDelayPerSecond / FramesReceivedPerSecond->StatValue * 1000.0;
				return FStatData(PixelStreaming2StatNames::JitterBufferDelay, MeanJitterBufferDelayMs, 2);
			}
			return {};
		});
	}

	void FRTCStatsCollector::FRTPRemoteTrackStatsSink::Process(const EpicRtcRemoteTrackRtpStats& InStats, const FString& PeerId, double SecondsDelta)
	{
		FStats* PSStats = FStats::Get();
		if (!PSStats)
		{
			return;
		}

		for (TPair<FName, FRTCTrackedStat>& Tuple : Stats)
		{
			double NewValue = 0;
			if (Tuple.Key == PixelStreaming2StatNames::FirCount)
			{
				NewValue = InStats._local._firCount;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::PliCount)
			{
				NewValue = InStats._local._pliCount;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::NackCount)
			{
				NewValue = InStats._local._nackCount;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::RetransmittedBytesReceived)
			{
				NewValue = InStats._local._retransmittedBytesReceived;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::RetransmittedPacketsReceived)
			{
				NewValue = InStats._local._retransmittedPacketsReceived;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::KeyFramesDecoded)
			{
				NewValue = InStats._local._keyFramesDecoded;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::FrameWidth)
			{
				NewValue = InStats._local._frameWidth;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::FrameHeight)
			{
				NewValue = InStats._local._frameHeight;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::FramesReceived)
			{
				NewValue = InStats._local._framesReceived;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::BytesReceived)
			{
				NewValue = InStats._local._bytesReceived;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::QPSum)
			{
				NewValue = InStats._local._qpSum;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::FramesDecoded)
			{
				NewValue = InStats._local._framesDecoded;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::PacketsLost)
			{
				NewValue = InStats._local._packetsLost;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::Jitter)
			{
				NewValue = InStats._local._jitter;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::RoundTripTime)
			{
				NewValue = InStats._remote._roundTripTime;
			}

			if (UpdateValue(NewValue, &Tuple.Value))
			{
				PSStats->StorePeerStat(PeerId, Category, Tuple.Value.GetLatestStat());
			}
		}

		PostProcess(PSStats, PeerId, SecondsDelta);
	}

	/**
	 * ---------- FRTPVideoSourceSink ----------
	 */
	FRTCStatsCollector::FVideoSourceStatsSink::FVideoSourceStatsSink(FName InCategory)
		: FStatsSink(InCategory)
	{
		// Track video source fps
		Add(PixelStreaming2StatNames::SourceFps, 0);
	}

	void FRTCStatsCollector::FVideoSourceStatsSink::Process(const EpicRtcVideoSourceStats& InStats, const FString& PeerId, double SecondsDelta)
	{
		FStats* PSStats = FStats::Get();
		if (!PSStats)
		{
			return;
		}

		for (TPair<FName, FRTCTrackedStat>& Tuple : Stats)
		{
			double NewValue = 0;
			if (Tuple.Key == PixelStreaming2StatNames::SourceFps)
			{
				NewValue = InStats._framesPerSecond;
			}

			if (UpdateValue(NewValue, &Tuple.Value))
			{
				PSStats->StorePeerStat(PeerId, Category, Tuple.Value.GetLatestStat());
			}
		}

		PostProcess(PSStats, PeerId, SecondsDelta);
	}

	/**
	 * ---------- FRTPAudioSourceSink ----------
	 */
	FRTCStatsCollector::FAudioSourceStatsSink::FAudioSourceStatsSink(FName InCategory)
		: FStatsSink(InCategory)
	{
		Add(PixelStreaming2StatNames::AudioLevel, 0);
		Add(PixelStreaming2StatNames::TotalSamplesDuration, 0);
	}

	void FRTCStatsCollector::FAudioSourceStatsSink::Process(const EpicRtcAudioSourceStats& InStats, const FString& PeerId, double SecondsDelta)
	{
		FStats* PSStats = FStats::Get();
		if (!PSStats)
		{
			return;
		}

		for (TPair<FName, FRTCTrackedStat>& Tuple : Stats)
		{
			double NewValue = 0;
			if (Tuple.Key == PixelStreaming2StatNames::AudioLevel)
			{
				NewValue = InStats._audioLevel;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::TotalSamplesDuration)
			{
				NewValue = InStats._totalSamplesDuration;
			}

			if (UpdateValue(NewValue, &Tuple.Value))
			{
				PSStats->StorePeerStat(PeerId, Category, Tuple.Value.GetLatestStat());
			}
		}

		PostProcess(PSStats, PeerId, SecondsDelta);
	}

	/**
	 * ----------- FDataChannelSink -----------
	 */
	FRTCStatsCollector::FDataTrackStatsSink::FDataTrackStatsSink(FName InCategory)
		: FStatsSink(InCategory)
	{
		// These names are added as aliased names because `bytesSent` is ambiguous stat that is used across inbound-rtp, outbound-rtp, and data-channel
		// so to disambiguate which state we are referring to we record the `bytesSent` stat for the data-channel but store and report it as `data-channel-bytesSent`
		AddAliased(PixelStreaming2StatNames::MessagesSent, PixelStreaming2StatNames::DataChannelMessagesSent, 0, FStatData::EDisplayFlags::TEXT | FStatData::EDisplayFlags::GRAPH);
		AddAliased(PixelStreaming2StatNames::MessagesReceived, PixelStreaming2StatNames::DataChannelBytesReceived, 0, FStatData::EDisplayFlags::TEXT | FStatData::EDisplayFlags::GRAPH);
		AddAliased(PixelStreaming2StatNames::BytesSent, PixelStreaming2StatNames::DataChannelBytesSent, 0, FStatData::EDisplayFlags::TEXT | FStatData::EDisplayFlags::GRAPH);
		AddAliased(PixelStreaming2StatNames::BytesReceived, PixelStreaming2StatNames::DataChannelMessagesReceived, 0, FStatData::EDisplayFlags::TEXT | FStatData::EDisplayFlags::GRAPH);
	}

	void FRTCStatsCollector::FDataTrackStatsSink::Process(const EpicRtcDataTrackStats& InStats, const FString& PeerId, double SecondsDelta)
	{
		FStats* PSStats = FStats::Get();
		if (!PSStats)
		{
			return;
		}

		for (TPair<FName, FRTCTrackedStat>& Tuple : Stats)
		{
			double NewValue = 0;
			if (Tuple.Key == PixelStreaming2StatNames::MessagesSent)
			{
				NewValue = InStats._messagesSent;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::MessagesReceived)
			{
				NewValue = InStats._messagesReceived;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::BytesSent)
			{
				NewValue = InStats._bytesSent;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::BytesReceived)
			{
				NewValue = InStats._bytesReceived;
			}

			if (UpdateValue(NewValue, &Tuple.Value))
			{
				PSStats->StorePeerStat(PeerId, Category, Tuple.Value.GetLatestStat());
			}
		}

		PostProcess(PSStats, PeerId, SecondsDelta);
	}

	/**
	 * ---------- FRTPAudioSourceSink ----------
	 */
	FRTCStatsCollector::FCandidatePairStatsSink::FCandidatePairStatsSink(FName InCategory)
		: FStatsSink(InCategory)
	{
		Add(PixelStreaming2StatNames::AvailableOutgoingBitrate, 0);
		Add(PixelStreaming2StatNames::AvailableIncomingBitrate, 0);
	}

	void FRTCStatsCollector::FCandidatePairStatsSink::Process(const EpicRtcIceCandidatePairStats& InStats, const FString& PeerId, double SecondsDelta)
	{
		FStats* PSStats = FStats::Get();
		if (!PSStats)
		{
			return;
		}

		for (TPair<FName, FRTCTrackedStat>& Tuple : Stats)
		{
			double NewValue = 0;
			if (Tuple.Key == PixelStreaming2StatNames::AvailableOutgoingBitrate)
			{
				NewValue = InStats._availableOutgoingBitrate;
			}
			else if (Tuple.Key == PixelStreaming2StatNames::AvailableIncomingBitrate)
			{
				NewValue = InStats._availableIncomingBitrate;
			}

			if (UpdateValue(NewValue, &Tuple.Value))
			{
				PSStats->StorePeerStat(PeerId, Category, Tuple.Value.GetLatestStat());
			}
		}

		PostProcess(PSStats, PeerId, SecondsDelta);
	}

	/**
	 * ---------- FRTCTrackedStat -------------------
	 */
	FRTCStatsCollector::FRTCTrackedStat::FRTCTrackedStat(FName StatName, FName Alias, int NDecimalPlaces, uint8 DisplayFlags)
		: LatestStat(StatName, 0.0, NDecimalPlaces)
	{
		LatestStat.DisplayFlags = DisplayFlags;
		LatestStat.Alias = Alias;
	}

	double FRTCStatsCollector::FRTCTrackedStat::CalculateDelta(double Period) const
	{
		return (LatestStat.StatValue - PrevValue) * Period;
	}

	double FRTCStatsCollector::FRTCTrackedStat::Average() const
	{
		return (LatestStat.StatValue + PrevValue) * 0.5;
	}

	void FRTCStatsCollector::FRTCTrackedStat::SetLatestValue(double InValue)
	{
		PrevValue = LatestStat.StatValue;
		LatestStat.StatValue = InValue;
	}

	/**
	 * --------- FStatsSink ------------------------
	 */
	FRTCStatsCollector::FStatsSink::FStatsSink(FName InCategory)
		: Category(MoveTemp(InCategory))
	{
	}

	void FRTCStatsCollector::FStatsSink::AddAliased(FName StatName, FName AliasedName, int NDecimalPlaces, uint8 DisplayFlags)
	{
		FRTCTrackedStat Stat = FRTCTrackedStat(StatName, AliasedName, NDecimalPlaces, DisplayFlags);
		Stats.Add(StatName, Stat);
	}

	bool FRTCStatsCollector::FStatsSink::UpdateValue(double NewValue, FRTCTrackedStat* SetValueHere)
	{
		const bool bZeroInitially = SetValueHere->GetLatestStat().StatValue == 0.0;
		SetValueHere->SetLatestValue(NewValue);
		const bool bZeroStill = SetValueHere->GetLatestStat().StatValue == 0.0;
		return !(bZeroInitially && bZeroStill);
	}

	void FRTCStatsCollector::FStatsSink::PostProcess(FStats* PSStats, const FString& PeerId, double SecondsDelta)
	{
		for (auto& Calculator : Calculators)
		{
			TOptional<FStatData> OptStatData = Calculator(*this, SecondsDelta);
			if (OptStatData.IsSet())
			{
				FStatData& StatData = *OptStatData;
				CalculatedStats.Add(StatData.StatName, StatData);
				PSStats->StorePeerStat(PeerId, Category, StatData);
			}
		}
	}
} // namespace UE::PixelStreaming2