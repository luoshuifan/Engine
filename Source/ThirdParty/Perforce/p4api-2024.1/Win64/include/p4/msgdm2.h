/*
 * Copyright 2019 Perforce Software.  All rights reserved.
 *
 * This file is part of Perforce - the FAST SCM System.
 */
/*
 * msgdm2.h - overflow definitions of errors for data manager core subsystem.
 */
class MsgDm2 {
    public:
	static ErrorId ExistingStorage;
	static ErrorId ConfigHistData;
	static ErrorId LbrScanBadState;
	static ErrorId LbrScanCtlNotFound;

	static ErrorId IntegIntoReadOnlyOverlay;
	static ErrorId IntegRequiresStream;
	static ErrorId IntegStreamSyntaxHint;

	static ErrorId OpenReadOnlyOverlay;
	static ErrorId OpenWarnOverlay;
	static ErrorId OpenUndoConflict;

	static ErrorId UnshelveBadOverlay;
	static ErrorId UnshelveStreamResolve;

	static ErrorId RequiresAutoIdCode;
	static ErrorId SpecMissingBuiltin;
	static ErrorId StreamSpecIntegOkay;
	static ErrorId CheckFailedNoDB;
	static ErrorId RequiresAutoIdOrPosCode;
	static ErrorId CannotRecreateDeleteField;
	static ErrorId SpecRepairDisallowNNN; 
	static ErrorId SpecRepairNoCustomSpec; 
	static ErrorId NoStreamSpecPermsWarn;
	static ErrorId StreamSpecProtectsNotCompatible;
	static ErrorId StreamOpenedByUser;
	static ErrorId StreamOpenReOpen;
	static ErrorId RemoteLabelOpenFailed;
	static ErrorId RemoteLabelUpdateFailed;
	static ErrorId RemoteStreamUpdateFailed;
	static ErrorId StreamAtChangeDeleted;
	static ErrorId StreamNotOpenInChange;
	static ErrorId StreamParentViewNoChange;
	static ErrorId LbrRevVerOutOfRange;
	static ErrorId GblLockIndexMismatch;
	static ErrorId GblLockIndexMissing;
	static ErrorId GblLockMissing;
	static ErrorId GblUnlockMissing;
	static ErrorId StreamlogInteg;
	static ErrorId RemoteAutoGenSpecFailed;
	static ErrorId StreamParentViewMustBeOpen;
	static ErrorId StreamPVSourceComment;
	static ErrorId BeginUpgradeStep;
	static ErrorId EndUpgradeStep;
	static ErrorId StreamNoCmtClientBadSave;
	static ErrorId ConnNeedsFwdCrypto;
	static ErrorId NoStreamTypeChangePV;
	static ErrorId PurgeTaskStream;
	static ErrorId PurgeCheckWldDelIgn;
	static ErrorId PurgeCheckWldDel;
	static ErrorId PurgeCheckIgn;
	static ErrorId PurgePurgeCheckWldDelIgn;
	static ErrorId PurgePurgeCheckWldDel;
	static ErrorId PurgePurgeCheckIgn;
	static ErrorId IdHasWhitespace;
	static ErrorId IdHasEquals;
	static ErrorId RmtAddTopologyFailed;
	static ErrorId RmtTopologyExists;
	static ErrorId ImportDittoGraph;
	static ErrorId ReopenHasMoved;
	static ErrorId TopologyData;
	static ErrorId StreamViewMatchData;
	static ErrorId NoTopologyRecord;
	static ErrorId NoServerIDSet;
	static ErrorId NoPartitionedToReadonly;
	static ErrorId TopologyRecDeleted;
	static ErrorId TopologyRecNotFound;
	static ErrorId LockNameNull;
	static ErrorId WorkRecNotFound;
	static ErrorId StreamDeletedInChange;
	static ErrorId DomainObliterate;
	static ErrorId StreamNotModifiedAtChange;
	static ErrorId PurgeStreamSpec;
	static ErrorId CannotDeleteShelvedStream;
	static ErrorId RmtArchiveDeleteFailed;
	static ErrorId RmtDeleteEdgeArchiveFailed;
	static ErrorId ComponentStreamInvalid;
	static ErrorId ComponentTypeNotAvailable;
	static ErrorId TopologyDelPreview;
	static ErrorId StreamHasComponentsDelete;
	static ErrorId StreamHasComponentsOblit;
	static ErrorId ComponentInvalidIsStream;
	static ErrorId ComponentInvalidIsConsumer;
	static ErrorId ComponentInvalidIsRelative;
	static ErrorId ReparentFailedParentIsComponent;
	static ErrorId ReparentFailedParentIsCompOfChild;
	static ErrorId ReparentFailedFamilyIsComponent;
	static ErrorId ReparentFailedFamilyIsCompOfChild;
	static ErrorId ReparentFailedParentHasComponent;
	static ErrorId ReparentFailedFamilyHasComponent;
	static ErrorId StreamDeletedInChangeWarn;
	static ErrorId StreamLoopFound;
	static ErrorId ComponentInvalidIsDependent;
	static ErrorId TopologyThresholdOutOfRange;
	static ErrorId ProtectsMismatch;
	static ErrorId DirsDataStreamViews;
	static ErrorId FilesDataStreamViews;
	static ErrorId MaxMem;
	static ErrorId GroupsDataVerbose222;
	static ErrorId TopologyDelRecMarker;
	static ErrorId TopologyMoveRecMarker;
	static ErrorId TopologyAmbiguity;
	static ErrorId TopologyTargetDeleted;
	static ErrorId TopologyRecAlreadyDeleted;
	static ErrorId ComponentWritableNoChange;
	static ErrorId NoChangeOnDistribution;
	static ErrorId TopologyRecAlreadyMoved;
	static ErrorId TopologyRecMoved;
	static ErrorId TopologyMoveSame;
	static ErrorId TopologyFailedToMark;
	static ErrorId TopologyMarkedMoveTo;
	static ErrorId NoAltSyncChangeWithHave;
	static ErrorId MaxMemOS;
	static ErrorId BadLazyPipeCount;
	static ErrorId StreamFieldValueError;
	static ErrorId StreamViewGenAtChangeSkip; 
	static ErrorId DepotTraitDup;
	static ErrorId NoTraitDepot;

	static ErrorId ComponentStreamInvalidSparse;
	static ErrorId NoReparentSparse;
	static ErrorId NoSparseChildren;
	static ErrorId NoStreamTypeChangeToNonSparse;
	static ErrorId NoStreamTypeChangeToSparse;
	static ErrorId NoVirtualParentSparseChild;
	static ErrorId SparseStreamCmdChangeSpecifierOnly;
	static ErrorId SparseStreamCmdMustIncludeAll;
	static ErrorId SparseStreamCmdMustIncludeAll2;
	static ErrorId SparseStreamCmdWrongStream;
	static ErrorId SparseStreamCmdNoRevRange;
	static ErrorId SparseStreamCopyOutOfDate;
	static ErrorId SparseStreamCopyLatestOnly;
	static ErrorId SparseStreamNoPin;
	static ErrorId SparseStreamNotSupported;
	static ErrorId SparseStreamOpNotAllowed;
	static ErrorId SparseStreamPinChangeDown;
	static ErrorId SparseStreamPinChangeUp;
	static ErrorId SparseStreamPinUpdate;
	static ErrorId VirtChildSparseParentOpNotAllowed;
	static ErrorId UnsubmitNoStreamSpec;

	static ErrorId OnlyOneClientReload;
	static ErrorId UnknownParam;
	static ErrorId BadS3Mode;
	static ErrorId DepotBadAddress;
	static ErrorId DepotBadRemoteAddress;
	static ErrorId NoConfigHistory;
	static ErrorId SyncToRemove;
	static ErrorId DomainNoViewCmtClientBadSave;
	static ErrorId ConfigNotFound;

	static ErrorId ChunkMapFormat;
} ;
