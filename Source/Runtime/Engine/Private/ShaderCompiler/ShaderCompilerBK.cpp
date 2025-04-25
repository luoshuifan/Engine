// ++ to support bk_dist accelerate

#include "ShaderCompiler.h"
#include "Misc/ConfigCacheIni.h"
#include "GenericPlatform/GenericPlatformFile.h"
#if PLATFORM_WINDOWS || PLATFORM_MAC
#include "HAL/PlatformFilemanager.h"
#else
#include "HAL/PlatformFile.h"
#endif
#include "HAL/FileManager.h"
#include "Misc/ScopeLock.h"
#include "Misc/FileHelper.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "Serialization/JsonSerializer.h"
#include <time.h>
#include "Runtime/Networking/Public/Networking.h"


namespace BKDistConsoleVariables
{
	int32 Enabled = 1;
	FAutoConsoleVariableRef CVarBKDistShaderCompile(
		TEXT("r.BKDistShaderCompile"),
		Enabled,
		TEXT("Enables or disables the use of BKDist to build shaders.\n")
		TEXT("0: Local builds only. \n")
		TEXT("1: Distribute builds using BKDist."),
		ECVF_Default);

	/** The maximum number of shaders to group into a single BKDist task. */
	int32 BatchSize = 5;
	FAutoConsoleVariableRef CVarBKDistShaderCompileBatchSize(
		TEXT("r.BKDistShaderCompile.BatchSize"),
		BatchSize,
		TEXT("Specifies the number of shaders to batch together into a single BKDist task.\n")
		TEXT("Default = 5\n"),
		ECVF_Default);

	/** The total number of batches to fill with shaders before creating another group of batches. */
	int32 BatchGroupSize = 200;
	FAutoConsoleVariableRef CVarBKDistShaderCompileBatchGroupSize(
		TEXT("r.BKDistShaderCompile.BatchGroupSize"),
		BatchGroupSize,
		TEXT("Specifies the number of batches to fill with shaders.\n")
		TEXT("Shaders are spread across this number of batches until all the batches are full.\n")
		TEXT("This allows the BKDist compile to go wider when compiling a small number of shaders.\n")
		TEXT("Default = 200\n"),
		ECVF_Default);

	/**
	* The number of seconds to wait after a job is submitted before kicking off the BKDist process.
	* This allows time for the engine to enqueue more shaders, so we get better batching.
	*/
	float JobTimeout = 0.1f;
	FAutoConsoleVariableRef CVarBKDistShaderCompileJobTimeout(
		TEXT("r.BKDistShaderCompile.JobTimeout"),
		JobTimeout,
		TEXT("The number of seconds to wait for write next jobs json file.\n")
		TEXT("Default = 1.0\n"),
		ECVF_Default);

	void Init()
	{
		static bool bInitialized = false;
		if (!bInitialized)
		{
			bool bAllowCompilingViaBKDist = true;
			GConfig->GetBool(TEXT("DevOptions.Shaders"), TEXT("bAllowDistributedCompilingWithBKDist"), bAllowCompilingViaBKDist, GEngineIni);

			BKDistConsoleVariables::Enabled = bAllowCompilingViaBKDist ? 1 : 0;

			// Allow command line to override the value of the console variables.
			if (FParse::Param(FCommandLine::Get(), TEXT("bkdistshadercompile")))
			{
				BKDistConsoleVariables::Enabled = 1;
			}

			static const IConsoleVariable* CVarAllowCompilingThroughWorkers = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shaders.AllowCompilingThroughWorkers"), false);
			if (FParse::Param(FCommandLine::Get(), TEXT("nobkdistshadercompile")) || FParse::Param(FCommandLine::Get(), TEXT("noshaderworker")) || (CVarAllowCompilingThroughWorkers && CVarAllowCompilingThroughWorkers->GetInt() == 0))
			{
				BKDistConsoleVariables::Enabled = 0;
			}

			bInitialized = true;
		}
	}
}

namespace BKDistShaderCompiling
{
	static const FString BKDist_InputFileSuffix(TEXT("Worker.in"));
	static const FString BKDist_OutputFileSuffix(TEXT("Worker.out"));

#if BK_DIST_DYNAMIC_PORT
	static const int32 BKShaderPort = 0;
	static int32 BKShaderDynamicPort = 0;
	#if PLATFORM_WINDOWS
		static const FString BKDist_ProcessInfo(TEXT("C:\\bk_dist\\bk-shader-tool-process.json"));
	#elif PLATFORM_MAC
		static const FString BKDist_ProcessInfo(TEXT("/Users/Shared/bk-shader-tool-process.json"));
	#else
		static const FString BKDist_ProcessInfo(TEXT("/etc/bk_dist/bk-shader-tool-process.json"));
	#endif
#else
	static const int32 BKShaderPort = 30118;
#endif


	static FArchive* CreateFileHelper(const FString& Filename)
	{
		// TODO: This logic came from FShaderCompileThreadRunnable::WriteNewTasks().
		// We can't avoid code duplication unless we refactored the local worker too.

		FArchive* File = nullptr;
		int32 RetryCount = 0;
		// Retry over the next two seconds if we can't write out the file.
		// Anti-virus and indexing applications can interfere and cause this to fail.
		while (File == nullptr && RetryCount < 200)
		{
			if (RetryCount > 0)
			{
				FPlatformProcess::Sleep(0.01f);
			}
			File = IFileManager::Get().CreateFileWriter(*Filename, FILEWRITE_EvenIfReadOnly);
			RetryCount++;
		}
		if (File == nullptr)
		{
			File = IFileManager::Get().CreateFileWriter(*Filename, FILEWRITE_EvenIfReadOnly | FILEWRITE_NoFail);
		}
		checkf(File, TEXT("Failed to create file %s!"), *Filename);
		return File;
	}

	static void MoveFileHelper(const FString& To, const FString& From)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

		if (PlatformFile.FileExists(*From))
		{
			FString DirectoryName;
			int32 LastSlashIndex;
			if (To.FindLastChar('/', LastSlashIndex))
			{
				DirectoryName = To.Left(LastSlashIndex);
			}
			else
			{
				DirectoryName = To;
			}

			// TODO: This logic came from FShaderCompileThreadRunnable::WriteNewTasks().
			// We can't avoid code duplication unless we refactored the local worker too.

			bool Success = false;
			int32 RetryCount = 0;
			// Retry over the next two seconds if we can't move the file.
			// Anti-virus and indexing applications can interfere and cause this to fail.
			while (!Success && RetryCount < 200)
			{
				if (RetryCount > 0)
				{
					FPlatformProcess::Sleep(0.01f);
				}

				// MoveFile does not create the directory tree, so try to do that now...
				Success = PlatformFile.CreateDirectoryTree(*DirectoryName);
				if (Success)
				{
					Success = PlatformFile.MoveFile(*To, *From);
				}
				RetryCount++;
			}
			checkf(Success, TEXT("Failed to move file %s to %s!"), *From, *To);
		}
	}

	static void DeleteFileHelper(const FString& Filename)
	{
		// TODO: This logic came from FShaderCompileThreadRunnable::WriteNewTasks().
		// We can't avoid code duplication unless we refactored the local worker too.
		if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*Filename))
		{
			bool bDeletedOutput = IFileManager::Get().Delete(*Filename, false, true);

			int32 RetryCount = 0;
			while (!bDeletedOutput && RetryCount < 50)
			{
				FPlatformProcess::Sleep(0.01f);
				bDeletedOutput = IFileManager::Get().Delete(*Filename, false, true);
				RetryCount++;
			}
			//checkf(bDeletedOutput, TEXT("Failed to delete %s!"), *Filename);
		}
	}

	static FString bkToolDir(TEXT(""));
	static FString bkToolController(TEXT(""));
	static FString bkToolShader(TEXT(""));

	static bool hasSetBKToolsDir = false;
	static bool hasBKToolsExisted = false;
	static bool hasBKToolChecked = false;
	static bool hasBKSwitchFileExisted = false;
	static bool hasBKSwitchFileChecked = false;

#if PLATFORM_WINDOWS
	bool InstalledWindowsMetal = false;
	// from : Engine\Source\Developer\Apple\MetalShaderFormat\Private\MetalShaderFormat.cpp : DoWindowsSetup()
	bool IsWindowsMetalInstalled()
	{
		int32 Result = 0;

		FString ToolchainBase;
		GConfig->GetString(TEXT("/Script/IOSRuntimeSettings.IOSRuntimeSettings"), TEXT("WindowsMetalToolchainOverride"), ToolchainBase, GEngineIni);

		const bool bUseOverride = (!ToolchainBase.IsEmpty() && FPaths::DirectoryExists(ToolchainBase));
		if (!bUseOverride)
		{
			ToolchainBase = TEXT("c:/Program Files/Metal Developer Tools");
		}

		// Look for the windows native toolchain
		FString Metal4Macos = ToolchainBase / TEXT("macos") / TEXT("bin") / TEXT("metal.exe");
		FString Metal4Ios = ToolchainBase / TEXT("ios") / TEXT("bin") / TEXT("metal.exe");

		return FPaths::FileExists(*Metal4Macos) && FPaths::FileExists(*Metal4Ios);
	}
#endif

	void addBKToolsDir2Path(FString& tempToolDir)
	{
		if (!hasSetBKToolsDir)
		{
			FString EnvVarValue = FPlatformMisc::GetEnvironmentVariable(TEXT("Path"));
			EnvVarValue += TEXT(";") + FPaths::ConvertRelativePathToFull(tempToolDir);
			FPlatformMisc::SetEnvironmentVar(TEXT("Path"), *EnvVarValue);

			hasSetBKToolsDir = true;
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] set Path=%s"), *EnvVarValue);
		}
	}

	bool checkTools(FString& tempToolDir)
	{
		if (tempToolDir.IsEmpty())
		{
			return false;
		}

#if PLATFORM_WINDOWS
		bkToolShader = tempToolDir / "bk-shader-tool.exe";
		bkToolShader = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*bkToolShader);
# else
		bkToolShader = tempToolDir / "bk-shader-tool";
		bkToolShader = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*bkToolShader);
#endif

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.FileExists(*bkToolShader))
		{
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] not found bk-shader-tool [%s] in dir [%s]."), *bkToolShader, *tempToolDir);
			return false;
		}
		else
		{
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] found bk-shader-tool [%s] in dir [%s]."), *bkToolShader, *tempToolDir);
			return true;
		}
	}

	bool isBKDistToolExisted()
	{
		if (hasBKToolChecked) {
			return hasBKToolsExisted;
		}

		hasBKToolChecked = true;

#if PLATFORM_WINDOWS
		bkToolDir = FPaths::EngineDir() + TEXT("Build/BatchFiles/bk_dist_tools");
#else 
		bkToolDir = FPaths::EngineDir() + TEXT("Build/BatchFiles/Mac/bk_dist_tools");
#endif
		if (checkTools(bkToolDir))
		{
			bkToolDir = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*bkToolDir);
			addBKToolsDir2Path(bkToolDir);
			hasBKToolsExisted = true;
			return true;
		}

		bkToolDir = FPlatformMisc::GetEnvironmentVariable(TEXT("BK_DIST_TOOLS_PATH"));
		if (checkTools(bkToolDir))
		{
			bkToolDir = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*bkToolDir);
			addBKToolsDir2Path(bkToolDir);
			hasBKToolsExisted = true;
			return true;
		}

		hasBKToolsExisted = false;
		return false;
	}

	bool isBKDistSwitchOn()
	{
		if (hasBKSwitchFileChecked)
		{
			return hasBKSwitchFileExisted;
		}

		hasBKSwitchFileChecked = true;

		FString switchFile = bkToolDir + TEXT("/bk_dist_enable.json");
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.FileExists(*switchFile))
		{
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] not found switch file [%s] in dir [%s]."), *switchFile, *bkToolDir);
			return false;
		}
		else
		{
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] found switch file [%s] in dir [%s]"), *switchFile, *bkToolDir);
			hasBKSwitchFileExisted = true;
			return true;
		}
	}

#if PLATFORM_MAC
	static bool bkSearchedMetal = false;
	static bool bkFoundMetal = false;
	static bool bkGEXcode16 = false;
	static FString bkMetalFrontendBinaryPath(TEXT(""));
	static FString bkMetalArBinaryPath(TEXT(""));
	static FString bkMetalLibraryBinaryPath(TEXT(""));
	static FString bkMetalMacosPath(TEXT(""));
	static FString bkMetalIosPath(TEXT(""));

	/*
	FString upperDir(FString inpath)
	{
		if (inpath.IsEmpty())
		{
			return inpath;
		}

		char pathchar = '/';
		int foundnum = 0;
		int len = inpath.Len();
		int pos = 0;
		for (int i = len - 1; i > 0; --i)
		{
			if (inpath[i] == pathchar)
			{
				++foundnum;
				if (foundnum == 2)
				{
					pos = i;
					break;
				}
			}
		}

		if (pos > 0)
		{
			return inpath.Left(pos + 1);
		}

		return inpath;
	}
	*/

	bool getRealMetalTools(FString firstexe)
	{
		int32 ReturnCode = 0;
		FString StdOut, StdErr;
		FString args(TEXT("-print-prog-name=metal"));
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] ready run cmd [%s %s] to search real metal"), *firstexe, *args);
		bool bSuccess = FPlatformProcess::ExecProcess(*firstexe, *args, &ReturnCode, &StdOut, &StdErr);
		StdOut = StdOut.TrimEnd();
		if (FPaths::FileExists(StdOut))
		{
			bkMetalFrontendBinaryPath = StdOut;
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] succeed to find real metal path[%s]"), *StdOut);
		} else {
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] real metal path[%s] not exist"), *StdOut);
			return false;
		}

		FString lastPath = FPaths::GetPath(bkMetalFrontendBinaryPath);
		bkMetalArBinaryPath = FPaths::Combine(lastPath, TEXT("metal-ar"));
		bkMetalLibraryBinaryPath = FPaths::Combine(lastPath, TEXT("metallib"));
		if (!FPaths::FileExists(bkMetalArBinaryPath) || !FPaths::FileExists(bkMetalLibraryBinaryPath))
		{
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] path[%s] or [%s] not exist"), *bkMetalArBinaryPath, *bkMetalLibraryBinaryPath);
			return false;
		}

		FString secondPath = FPaths::GetPath(lastPath);
		FString thirdPath = FPaths::GetPath(secondPath);

		// search as xocde before 16
		bkMetalMacosPath = FPaths::Combine(thirdPath, TEXT("macos"));
		bkMetalIosPath = FPaths::Combine(thirdPath, TEXT("ios"));
		if (FPaths::DirectoryExists(bkMetalMacosPath) && FPaths::DirectoryExists(bkMetalIosPath))
		{
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] succeed to find tool base path[%s] and [%s]"), *bkMetalMacosPath, *bkMetalIosPath);
			return true;
		}

		// search as xocde ge 16
		bkMetalMacosPath = FPaths::Combine(thirdPath, TEXT("current"));
		bkMetalIosPath = FPaths::Combine(thirdPath, TEXT("current"));
		if (FPaths::DirectoryExists(bkMetalMacosPath))
		{
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] succeed to find tool base path[%s]"), *bkMetalMacosPath);
			bkGEXcode16 = true;
			return true;
		}

		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] not found tool base path[%s]"), *bkMetalMacosPath);
		return false;
	}

	// obtain metal/metal-ar/metallib
	bool isMacMetalFilesExisted()
	{
		if (bkSearchedMetal)
		{
			return bkFoundMetal;
		}

		bkSearchedMetal = true;
		bkFoundMetal = false;

		int32 ReturnCode = 0;
		FString StdOut, StdErr;
		FString XcrunPath(TEXT("/usr/bin/xcrun"));
		FString args(TEXT("-sdk macosx --find metal"));
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] ready run cmd [%s %s] to search metal"), *XcrunPath, *args);
		bool bSuccess = FPlatformProcess::ExecProcess(*XcrunPath, *args, &ReturnCode, &StdOut, &StdErr);
		StdOut = StdOut.TrimEnd();
		if (FPaths::FileExists(StdOut))
		{
			/*
			FString upperpath = upperDir(StdOut);
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] succeed to find metal path[%s],upper[%s]"), *StdOut, *upperpath);

			bkMetalFrontendBinaryPath = upperpath + "metal/macos/bin/metal";
			bkMetalArBinaryPath = upperpath + "metal/macos/bin/metal-ar";
			bkMetalLibraryBinaryPath = upperpath + "metal/macos/bin/metallib";

			bkFoundMetal = FPaths::FileExists(bkMetalFrontendBinaryPath) && FPaths::FileExists(bkMetalArBinaryPath) && FPaths::FileExists(bkMetalLibraryBinaryPath);

			if (bkFoundMetal)
			{
				bkMetalMacosPath = upperpath + "metal/macos";
				bkMetalIosPath = upperpath + "metal/ios";
				UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] succeed to find real metal path [%s]"), *bkMetalFrontendBinaryPath);
			}
			else
			{
				UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] failed to find real metal path [%s][%s][%s]"), *bkMetalFrontendBinaryPath, *bkMetalArBinaryPath, *bkMetalLibraryBinaryPath);
			}
			return bkFoundMetal;
			*/
			bkFoundMetal = getRealMetalTools(StdOut);
		}

		//UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] find metal path [%s] with retcode:%d,error:%s"), *StdOut, ReturnCode, *StdErr);
		return bkFoundMetal;
	}
#endif

	bool isBKDistEnabled()
	{
#if PLATFORM_WINDOWS
		return isBKDistToolExisted() && isBKDistSwitchOn();
#elif PLATFORM_MAC
		return isBKDistToolExisted() && isBKDistSwitchOn() && isMacMetalFilesExisted();
#else
		return false;
#endif
	}

	class FDependencyEnumerator : public IPlatformFile::FDirectoryVisitor
	{
	public:
		FDependencyEnumerator(TArray<FString>* InArray, const TCHAR* InPrefix, const TCHAR* InExtension, const TCHAR* InExcludeExtensions = NULL)
			: array(InArray)
			, Prefix(InPrefix)
			, Extension(InExtension)
		{
			if (InExcludeExtensions != NULL)
			{
				FString Extersion = InExcludeExtensions;
				while (true)
				{
					FString CurExt = TEXT("");
					FString LeftExts = TEXT("");
					if (Extersion.Split(TEXT("|"), &CurExt, &LeftExts))
					{
						ExcludedExtersions.Add(CurExt);
						Extersion = LeftExts;
					}
					else
					{
						ExcludedExtersions.Add(Extersion);
						break;
					}
				}
			}
		}

		// can't return false, otherwise it will break dir iteration
		virtual bool Visit(const TCHAR* FilenameChar, bool bIsDirectory) override
		{
			if (!bIsDirectory)
			{
				FString FileExtension = FPaths::GetExtension(FilenameChar);
				bool Excluded = ExcludedExtersions.Find(FileExtension) != INDEX_NONE;
				if (Excluded)
				{
					// UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] do not include file [%s] with it's extension"), FilenameChar);
					return true;
				}

				FString FileName = FPaths::GetBaseFilename(FilenameChar);
				if ((!Prefix || FileName.StartsWith(Prefix)) && (!Extension || FileExtension.Equals(Extension)))
				{
					FString inputFile = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(FilenameChar);
					array->Push(inputFile);
				}
			}

			return true;
		}

		TArray<FString>* array;
		const TCHAR* Prefix;
		const TCHAR* Extension;
		TArray<FString> ExcludedExtersions;
	};

	void FindFiles(TArray<FString>* FileNames, const TCHAR* StartDirectory, const TCHAR* InPrefix, const TCHAR* InExtension, const TCHAR* InExcludeExtensions = NULL, TArray<FString>* ExcludedDirs = NULL)
	{
		FString CurrentSearch = FString(StartDirectory) / TEXT("*");
		TArray<FString> Result;
		IFileManager::Get().FindFiles(Result, *CurrentSearch, true, false);

		FDependencyEnumerator visitor = FDependencyEnumerator(FileNames, InPrefix, InExtension, InExcludeExtensions);

		// save files in current dir
		for (int32 i = 0; i < Result.Num(); i++)
		{
			FString SubFile = FString(StartDirectory) / Result[i];
			visitor.Visit(*SubFile, false);
		}

		// iterate sub-dirs in current dir
		TArray<FString> SubDirs;
		FString RecursiveDirSearch = FString(StartDirectory) / TEXT("*");
		IFileManager::Get().FindFiles(SubDirs, *RecursiveDirSearch, false, true);

		for (int32 SubDirIdx = 0; SubDirIdx < SubDirs.Num(); SubDirIdx++)
		{
			// UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] check subdir [%s]"), *(SubDirs[SubDirIdx]));

			if (ExcludedDirs != NULL && ExcludedDirs->Find(SubDirs[SubDirIdx]) != INDEX_NONE)
			{
				UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] subdir %s is exclude"), *(SubDirs[SubDirIdx]));
				continue;
			}

			FString SubDir = FString(StartDirectory) / SubDirs[SubDirIdx];
			if (SubDir.Find(TEXT(".svn")) != INDEX_NONE || SubDir.Find(TEXT(".git")) != INDEX_NONE)
			{
				// UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] do not include dir [%s] with path [.svn or .git]"), *SubDir);
				continue;
			}

			FindFiles(FileNames, *SubDir, InPrefix, InExtension, InExcludeExtensions, ExcludedDirs);
		}
	}

	FString getRelativePath(FString& absPath)
	{
		FString pathWithouDrive = absPath;
		if (absPath.Len() > 3 && absPath[1] == ':')
		{
			pathWithouDrive = absPath.Right(absPath.Len() - 3);
		}

		return FPaths::Combine(TEXT("{{task_id}}"), FPaths::GetPath(pathWithouDrive));
	}

	FString getDriver(FString& absPath)
	{
#if PLATFORM_WINDOWS
		FString pathDrive = "";
		if (absPath.Len() > 2 && absPath[1] == ':')
		{
			pathDrive = absPath.Left(2);
		}
		return pathDrive;
#else
		return "/";
#endif
	}

#if PLATFORM_WINDOWS
	static const FString ToolchainRoot[]
	{
		TEXT("Engine\\Binaries\\ThirdParty\\Windows\\DirectX\\x64"),
		TEXT("Engine\\Binaries\\ThirdParty\\AppLocalDependencies\\Win64"),
		TEXT("Engine\\Binaries\\ThirdParty\\ShaderConductor\\Win64"),
	};
#elif PLATFORM_MAC
	static const FString ToolchainRoot[]{
		TEXT("Engine\\Binaries\\ThirdParty\\ShaderConductor\\Mac"),
		TEXT("Engine\\Binaries\\ThirdParty\\Intel\\TBB\\Mac"),
		TEXT("Engine\\Binaries\\ThirdParty\\Apple\\MetalShaderConverter\\Mac"),
	};
#else
	static const FString ToolchainRoot[]{};
#endif

	static const FString ShaderPluginsRoot[]
	{
		TEXT("Engine\\Plugins"),
	};

	static const FString ConfigRoot[]
	{
		TEXT("Engine\\Config")
	};

	void getAllInputFiles(TArray<FString>* inputFiles)
	{
		// UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] get all input files in..."));

#if PLATFORM_WINDOWS
		for (const FString& ExtraFilePartialPath : ToolchainRoot)
			FindFiles(inputFiles, *FPaths::Combine(*FPaths::RootDir(), *ExtraFilePartialPath), NULL, NULL, NULL);
#elif PLATFORM_MAC
		TArray<FString> ExcludedDirs;
		ExcludedDirs.Add(TEXT("DWARF"));
		for (const FString& ExtraFilePartialPath : ToolchainRoot)
			FindFiles(inputFiles, *FPaths::Combine(*FPaths::RootDir(), *ExtraFilePartialPath), NULL, NULL, TEXT("dSYM"), &ExcludedDirs);
#endif

#if PLATFORM_WINDOWS
		FindFiles(inputFiles, *FPlatformProcess::GetModulesDirectory(), TEXT("ShaderCompileWorker"), NULL, TEXT("exe|EXE|pdb|PDB"));
#elif PLATFORM_MAC
		FindFiles(inputFiles, *FPlatformProcess::GetModulesDirectory(), TEXT("ShaderCompileWorker"), NULL, NULL);
#endif

#if PLATFORM_WINDOWS
		FindFiles(inputFiles, *FPlatformProcess::GetModulesDirectory(), TEXT("dx"), TEXT("dll"), NULL);

		FindFiles(inputFiles, *FPlatformProcess::GetModulesDirectory(), TEXT("ShaderConductor"), TEXT("dll"), NULL);
		FindFiles(inputFiles, *FPlatformProcess::GetModulesDirectory(), TEXT("tbbmalloc"), TEXT("dll"), NULL);
#endif
		const auto DirectoryMappings = AllShaderSourceDirectoryMappings();
		for (const auto& MappingEntry : DirectoryMappings)
		{
			UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] start to search files for dir[%s]"), *MappingEntry.Value);
			// add the map dir anyway, to avoid it is empty dir
			FString shaderdir = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*MappingEntry.Value);
			inputFiles->Push(shaderdir);
			FindFiles(inputFiles, *MappingEntry.Value, NULL, NULL, TEXT("uasset|umap|csv"));
			UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] finished serching files for dir[%s]"), *MappingEntry.Value);
		}

		//// add all plugins files
		//for (const FString& pluginPath : ShaderPluginsRoot)
		//{
		//	FindFiles(inputFiles, *FPaths::Combine(*FPaths::RootDir(), *pluginPath), NULL, TEXT("ush"), NULL);
		//	FindFiles(inputFiles, *FPaths::Combine(*FPaths::RootDir(), *pluginPath), NULL, TEXT("usf"), NULL);
		//}

		// add all config files
		for (const FString& path : ConfigRoot)
		{
			FindFiles(inputFiles, *FPaths::Combine(*FPaths::RootDir(), *path), NULL, TEXT("ini"), NULL);
		}

		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] get all input files out, got %d files,"), inputFiles->Num());

		// for debug, to log all input files
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] ----------------start log all input files-----------------"));
		/*for (const FString& f : *inputFiles)
		{
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] input file [%s]"), *f);
		}*/
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] ----------------end log all input files-----------------"));
	}

#if PLATFORM_MAC
	FString getMacosMetalRelativePath(FString& localAbsPath, FString& remoteWorkdir)
	{
		if (!bkGEXcode16)
		{
			FString onlypath = FPaths::GetPath(localAbsPath);
			int32 pos = onlypath.Find("/macos");
			if (pos > 0)
			{
				FString suffix = onlypath.Right(onlypath.Len() - pos);
				// UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] got macos metal file suffix %s"), *suffix);

				return remoteWorkdir + suffix;
			}

			return remoteWorkdir;
		} else {
			FString onlypath = FPaths::GetPath(localAbsPath);
			int32 pos = onlypath.Find("/current");
			if (pos > 0)
			{
				FString suffix = onlypath.Right(onlypath.Len() - pos);
				// UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] got macos metal file suffix %s"), *suffix);

				return remoteWorkdir + suffix;
			}

			return remoteWorkdir;
		}
	}

	FString getIosMetalRelativePath(FString& localAbsPath, FString& remoteWorkdir)
	{
		if (!bkGEXcode16)
		{
			FString onlypath = FPaths::GetPath(localAbsPath);
			int32 pos = onlypath.Find("/ios");
			if (pos > 0)
			{
				FString suffix = onlypath.Right(onlypath.Len() - pos);
				// UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] got ios metal file suffix %s"), *suffix);

				return remoteWorkdir + suffix;
			}

			return remoteWorkdir;
		} else {
			FString onlypath = FPaths::GetPath(localAbsPath);
			int32 pos = onlypath.Find("/current");
			if (pos > 0)
			{
				FString suffix = onlypath.Right(onlypath.Len() - pos);
				// UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] got ios metal file suffix %s"), *suffix);

				return remoteWorkdir + suffix;
			}

			return remoteWorkdir;
		}
	}

	void addMacMetalFiles(TArray<TSharedPtr<FJsonValue>>& InputArray, FString remotepath)
	{
		TArray<FString> macosinputFiles;
		FindFiles(&macosinputFiles, *bkMetalMacosPath, NULL, NULL, NULL);
		for (const FString& f : macosinputFiles)
		{
			TSharedPtr<FJsonObject> OneInputFileObj = MakeShareable(new FJsonObject);
			TSharedPtr<FJsonValue> OneInputFile = MakeShareable(new FJsonValueObject(OneInputFileObj));
			FString absPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*f);
			OneInputFile->AsObject()->SetStringField(TEXT("local_full_path"), *absPath);
			OneInputFile->AsObject()->SetStringField(TEXT("remote_relative_path"), *getMacosMetalRelativePath(absPath, remotepath));
			InputArray.Add(OneInputFile);
		}

		if (!bkGEXcode16)
		{
			TArray<FString> iosinputFiles;
			FindFiles(&iosinputFiles, *bkMetalIosPath, NULL, NULL, NULL);
			for (const FString& f : iosinputFiles)
			{
				TSharedPtr<FJsonObject> OneInputFileObj = MakeShareable(new FJsonObject);
				TSharedPtr<FJsonValue> OneInputFile = MakeShareable(new FJsonValueObject(OneInputFileObj));
				FString absPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*f);
				OneInputFile->AsObject()->SetStringField(TEXT("local_full_path"), *absPath);
				OneInputFile->AsObject()->SetStringField(TEXT("remote_relative_path"), *getIosMetalRelativePath(absPath, remotepath));
				InputArray.Add(OneInputFile);
			}
		}
	}
#endif

	// fill json file of toolchain
	TSharedPtr<FJsonObject> getToolchainJson(const FString& WorkerName)
	{
		uint64 t1 = time(NULL);
		uint64 t2 = t1;

		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] get toolchain json in at %d"), time(NULL));

		// obtain all input files
		TArray<FString> inputFiles;
		getAllInputFiles(&inputFiles);
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] finished get all input files"));

		t2 = time(NULL);
		if (t2 - t1 >= 1)
		{
			UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] over 1 seconds for getToolchainJson %d seconds to getAllInputFiles"), t2 - t1);
		}
		t1 = t2;

		// formate to json
		TSharedPtr<FJsonObject> RootObj = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> ToolArray;

		TSharedPtr<FJsonObject> OneToolChainObj = MakeShareable(new FJsonObject);
		TSharedPtr<FJsonValue> OneToolChain = MakeShareable(new FJsonValueObject(OneToolChainObj));
		FString workerAbsPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*WorkerName);
		OneToolChain->AsObject()->SetStringField(TEXT("tool_key"), *workerAbsPath);
		OneToolChain->AsObject()->SetStringField(TEXT("tool_name"), TEXT("ShaderCompileWorker.exe"));
		OneToolChain->AsObject()->SetStringField(TEXT("tool_local_full_path"), *workerAbsPath);
		OneToolChain->AsObject()->SetStringField(TEXT("tool_remote_relative_path"), *FPaths::GetPath(workerAbsPath));

		FString exeDirver = getDriver(workerAbsPath);
		FString exePath = FPaths::GetPath(workerAbsPath);

		TArray<TSharedPtr<FJsonValue>> InputArray;
		for (const FString& f : inputFiles)
		{
			TSharedPtr<FJsonObject> OneInputFileObj = MakeShareable(new FJsonObject);
			TSharedPtr<FJsonValue> OneInputFile = MakeShareable(new FJsonValueObject(OneInputFileObj));
			FString absPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*f);

			// ++ to support bk_dist accelerate, for tbs p2p
#if BK_DIST_RELATIVE_PATH && PLATFORM_WINDOWS
			FString SourcePath = absPath;
			FString DestinationPath;
			if (!FPaths::IsUnderDirectory(SourcePath, FPaths::RootDir()))
			{
				DestinationPath = FShaderCompileUtilities::RemapPath(SourcePath);

				IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
				FDateTime SourceTimeStamp = PlatformFile.GetTimeStamp(*SourcePath);
				FDateTime DestinationTimeStamp = PlatformFile.GetTimeStamp(*DestinationPath);

				if (SourceTimeStamp != FDateTime::MinValue() && SourceTimeStamp != DestinationTimeStamp)
				{
					FString DestinationDirectory = FPaths::GetPath(DestinationPath);
					if (!PlatformFile.DirectoryExists(*DestinationDirectory))
					{
						PlatformFile.CreateDirectoryTree(*DestinationDirectory);
					}
					PlatformFile.CopyFile(*DestinationPath, *SourcePath);
					PlatformFile.SetTimeStamp(*DestinationPath, SourceTimeStamp);
				}

				DestinationPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*DestinationPath);
				absPath = DestinationPath;
			}
#endif
			// -- to support bk_dist accelerate, for tbs p2p

			OneInputFile->AsObject()->SetStringField(TEXT("local_full_path"), *absPath);

			OneInputFile->AsObject()->SetStringField(TEXT("remote_relative_path"), *FPaths::GetPath(absPath));
			InputArray.Add(OneInputFile);
		}

#if PLATFORM_MAC
		// addMacMetalFiles(InputArray, getRelativePath(workerAbsPath));
		addMacMetalFiles(InputArray, FPaths::GetPath(workerAbsPath));
#endif

		OneToolChain->AsObject()->SetArrayField(TEXT("files"), InputArray);
		ToolArray.Add(OneToolChain);

		RootObj->SetArrayField(TEXT("toolchains"), ToolArray);

		return RootObj;
	}
}

#if PLATFORM_WINDOWS
// do not support Metal Developer Tools now
// all Valid target platforms: 
//		Win32,Win64,HoloLens,Mac,XboxOne,PS4,IOS,Android,HTML5,Linux,LinuxAArch64,AllDesktop,TVOS,Switch,Lumin
bool IsTargetPlatformSupported()
{
	// from : Engine\Source\Developer\TargetPlatform\Private\TargetPlatformManagerModule.cpp : GetActiveTargetPlatforms()
	FString PlatformStr;
	FParse::Value(FCommandLine::Get(), TEXT("TARGETPLATFORM="), PlatformStr);
	UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] BK-dist Shader Compiler found target platform [%s]"), *PlatformStr);
	if (PlatformStr == TEXT("All"))
	{
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] do not support target platform [%s]"), *PlatformStr);
		return false;
	}

	if (PlatformStr == TEXT("None"))
	{
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] support target platform [%s]"), *PlatformStr);
		return true;
	}

	TArray<FString> PlatformNames;
	PlatformStr.ParseIntoArray(PlatformNames, TEXT("+"), true);
	if (PlatformNames.Contains(TEXT("Mac")) ||
		PlatformNames.Contains(TEXT("IOS")) ||
		PlatformNames.Contains(TEXT("AllDesktop")) ||
		PlatformNames.Contains(TEXT("TVOS")))
	{
		// TODO : we can support Metal Developer Tools now later, but need send Metal Developer Tools to remote worker
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] do not support target platform [%s]"), *PlatformStr);
		return false;
	}

	return true;
}
#endif

bool FShaderCompileBKDistThreadRunnable::IsSupported()
{
#if PLATFORM_WINDOWS
	BKDistShaderCompiling::InstalledWindowsMetal = BKDistShaderCompiling::IsWindowsMetalInstalled();
	if (!IsTargetPlatformSupported())
	{
		return false;
	}
#endif

	BKDistConsoleVariables::Init();

	// Check to see if the FBuild exe exists.
	if (BKDistConsoleVariables::Enabled == 1)
	{
		// check bk_dist tools
		bool supported = BKDistShaderCompiling::isBKDistEnabled();
		if (supported)
		{
			// ensure module of "Sockets" loaded in game thread
			TSharedPtr<FInternetAddr> addr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
		}
		return supported;
	}
	else
	{
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] BKDist is disabled by the configuration 'bAllowDistributedCompilingWithBKDist' in BaseEngine.ini"));
	}

	return false;
}

/** Initialization constructor. */
FShaderCompileBKDistThreadRunnable::FShaderCompileBKDistThreadRunnable(class FShaderCompilingManager* InManager)
	: FShaderCompileThreadRunnableBase(InManager)
	, BuildProcessID(INDEX_NONE)
	, BKShaderToolID(0)
	, ShaderBatching(NULL)
	, WorkingDirectory(InManager->AbsoluteShaderBaseWorkingDirectory / TEXT("BKDist"))
	, BatchIndex(0)
	, DirectoryIndex(0)
	, LastCheckTime(0)
	, LastLaunchTime(0)
	, BKShaderAvailable(false)
	, BKCheckFailCounter(0)
	, InLaunching(false)
{
	ShaderBatchesInFlight.Reserve(BKDistConsoleVariables::BatchGroupSize);
	ShaderBatchesCompleted.Reserve(BKDistConsoleVariables::BatchGroupSize);
	ShaderBatchesFull.Reserve(BKDistConsoleVariables::BatchGroupSize);

	// init global var here
	FGuid temp;
	FPlatformMisc::CreateGuid(temp);
	Guid = temp.ToString();
}

FShaderCompileBKDistThreadRunnable::~FShaderCompileBKDistThreadRunnable()
{
	// do not kill bk-shader-tool
	//if (BuildProcessHandle.IsValid())
	//{
	//	// We still have a build in progress.
	//	// Kill it...
	//	FPlatformProcess::TerminateProc(BuildProcessHandle);
	//	FPlatformProcess::CloseProc(BuildProcessHandle);
	//}

	// Clean up any intermediate files/directories we've got left over.
	IFileManager::Get().DeleteDirectory(*WorkingDirectory, false, true);

	// Delete all the shader batch instances we have.
	for (FShaderBatch* Batch : ShaderBatchesCompleted)
		delete Batch;

	for (FShaderBatch* Batch : ShaderBatchesInFlight)
		delete Batch;

	for (FShaderBatch* Batch : ShaderBatchesFull)
		delete Batch;

	if (ShaderBatching)
	{
		delete ShaderBatching;
		ShaderBatching = NULL;
	}
	ShaderBatchesInFlight.Empty();
	ShaderBatchesFull.Empty();
	ShaderBatchesCompleted.Empty();
}

FShaderCompileBKDistThreadRunnable::FShaderBatch::FShaderBatch(const FString& InDirectoryBase, const FString& InInputFileSuffix, const FString& InOutputFileSuffix, const FString& InGuid, int32 InDirectoryIndex, int32 InBatchIndex)
	: bTransferFileWritten(false)
	, DirectoryBase(InDirectoryBase)
	, InputFileSuffix(InInputFileSuffix)
	, OutputFileSuffix(InOutputFileSuffix)
	, Guid(InGuid)
{
	SetIndices(InDirectoryIndex, InBatchIndex);
	Jobs.Reserve(BKDistConsoleVariables::BatchSize);
}

void FShaderCompileBKDistThreadRunnable::PostCompletedJobsForBatch(FShaderBatch* Batch)
{
	// Enter the critical section so we can access the input and output queues
	FScopeLock Lock(&Manager->CompileQueueSection);
	for (auto Job : Batch->GetJobs())
	{
		/*FShaderMapCompileResults& ShaderMapResults = Manager->ShaderMapJobs.FindChecked(Job->Id);
		ShaderMapResults.FinishedJobs.Add(Job);
		ShaderMapResults.bAllJobsSucceeded = ShaderMapResults.bAllJobsSucceeded && Job->bSucceeded;*/

		Manager->ProcessFinishedJob(Job.GetReference());
	}

	//// Using atomics to update NumOutstandingJobs since it is read outside of the critical section
	//FPlatformAtomics::InterlockedAdd(&Manager->NumOutstandingJobs, -Batch->NumJobs());
}

void FShaderCompileBKDistThreadRunnable::FShaderBatch::AddJob(FShaderCommonCompileJobPtr Job)
{
	// We can only add jobs to a batch which hasn't been written out yet.
	if (bTransferFileWritten)
	{
		UE_LOG(LogShaderCompilers, Fatal, TEXT("[bk_dist] Attempt to add shader compile jobs to a BKDist shader batch which has already been written to disk."));
	}
	else
	{
		Jobs.Add(Job);
	}
}

void FShaderCompileBKDistThreadRunnable::FShaderBatch::WriteTransferFile()
{
	// Write out the file that the worker app is waiting for, which has all the information needed to compile the shader.
	FArchive* TransferFile = BKDistShaderCompiling::CreateFileHelper(InputFileNameAndPath);
	// ++ to support bk_dist accelerate, for tbs p2p
#if BK_DIST_RELATIVE_PATH && PLATFORM_WINDOWS
	FShaderCompileUtilities::DoWriteTasks(Jobs, *TransferFile, nullptr, true);
#else
	FShaderCompileUtilities::DoWriteTasks(Jobs, *TransferFile);
#endif
	// -- to support bk_dist accelerate, for tbs p2p
	delete TransferFile;

	bTransferFileWritten = true;
}

void FShaderCompileBKDistThreadRunnable::FShaderBatch::SetIndices(int32 InDirectoryIndex, int32 InBatchIndex)
{
	DirectoryIndex = InDirectoryIndex;
	BatchIndex = InBatchIndex;

	WorkingDirectory = DirectoryBase;
	InputFileName = FString::Printf(TEXT("%s_%d_%d_%s"), *Guid, DirectoryIndex, BatchIndex, *InputFileSuffix);
	OutputFileName = FString::Printf(TEXT("%s_%d_%d_%s"), *Guid, DirectoryIndex, BatchIndex, *OutputFileSuffix);

	InputFileNameAndPath = FString::Printf(TEXT("%s/%s"), *WorkingDirectory, *InputFileName);
	OutputFileNameAndPath = FString::Printf(TEXT("%s/%s"), *WorkingDirectory, *OutputFileName);
}

void FShaderCompileBKDistThreadRunnable::FShaderBatch::CleanUpFiles(bool keepInputFile)
{
	if (!keepInputFile)
	{
		BKDistShaderCompiling::DeleteFileHelper(InputFileNameAndPath);
	}

	BKDistShaderCompiling::DeleteFileHelper(OutputFileNameAndPath);
}

void FShaderCompileBKDistThreadRunnable::GatherResultsFromBKDist()
{
	if (ShaderBatchesInFlight.Num() == 0)
	{
		return;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	IFileManager& FileManager = IFileManager::Get();

	// Reverse iterate so we can remove batches that have completed as we go.
	// for (int32 Index = ShaderBatchesInFlight.Num() - 1, counter = 0; Index >= 0; Index--,counter++)
	bool removed = false;
	for (int32 Index = 0; Index < ShaderBatchesInFlight.Num(); )
	{
		removed = false;
		FShaderBatch* Batch = ShaderBatchesInFlight[Index];

		// Perform the same checks on the worker output file to verify it came from this build.
		if (PlatformFile.FileExists(*Batch->OutputFileNameAndPath))
		{
			FArchive* OutputFileAr = FileManager.CreateFileReader(*Batch->OutputFileNameAndPath, FILEREAD_Silent);
			if (OutputFileAr)
			{
				FArchive& OutputFile = *OutputFileAr;
				FShaderCompileUtilities::DoReadTaskResults(Batch->GetJobs(), OutputFile);

				// Close the output file.
				delete OutputFileAr;

				PostCompletedJobsForBatch(Batch);
				ShaderBatchesInFlight.RemoveAt(Index, 1, false);
				removed = true;

				// clean up the batch files after process exited, avoid errors of deleting file (error code 32, which means the file was in processing with other program).
				ShaderBatchesCompleted.Add(Batch);
			}
		}

		// if removed, the element at Index is changed, so do not inc Index
		if (!removed)
		{
			++Index;
		}
	}
}

void FShaderCompileBKDistThreadRunnable::CleanCompleted()
{
	if (ShaderBatchesCompleted.Num() > 0)
	{
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] found [%d] tasks completed by bk-shader-tool during this period"), ShaderBatchesCompleted.Num());
		for (FShaderBatch* Batch : ShaderBatchesCompleted)
		{
			Batch->CleanUpFiles(false);
			delete Batch;
		}
		ShaderBatchesCompleted.Empty(BKDistConsoleVariables::BatchGroupSize);
	}
}

// fill json file of shaders
FString FShaderCompileBKDistThreadRunnable::GetShaderJson(const FString& WorkerName, uint32 InProcessId, TArray<FShaderBatch*>& shaders)
{
	uint64 t1 = time(NULL);
	uint64 t2 = t1;

	UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] get shader json in at %d"), time(NULL));

	TSharedPtr<FJsonObject> ToolObj = BKDistShaderCompiling::getToolchainJson(Manager->ShaderCompileWorkerName);
	TSharedPtr<FJsonValue> ToolObjValue = MakeShareable(new FJsonValueObject(ToolObj));

	TSharedPtr<FJsonObject> RootObj = MakeShareable(new FJsonObject);
	//RootObj->SetStringField(TEXT("tool_json_file"), *toolJsonFile);
	RootObj->SetField(TEXT("tool_json"), ToolObjValue);

	// common paramters
	FString WorkerParameters = FString(TEXT(" -xge_xml -communicatethroughfile "));
	if (GIsBuildMachine)
	{
		WorkerParameters += FString(TEXT(" -buildmachine "));
	}

	if (PLATFORM_LINUX) //-V560
	{
		// suppress log generation as much as possible
		WorkerParameters += FString(TEXT(" -logcmds=\"Global None\" "));

		if (UE_BUILD_DEBUG)
		{
			// when running a debug build under Linux, make SCW crash with core for easier debugging
			WorkerParameters += FString(TEXT(" -core "));
		}
	}
	WorkerParameters += FCommandLine::GetSubprocessCommandline();

	TArray<TSharedPtr<FJsonValue>> ShaderArray;
	for (FShaderBatch* Batch : shaders)
	{
		TSharedPtr<FJsonObject> OneInputFileObj = MakeShareable(new FJsonObject);
		TSharedPtr<FJsonValue> OneInputFile = MakeShareable(new FJsonValueObject(OneInputFileObj));
		OneInputFile->AsObject()->SetStringField(TEXT("cmd"), *WorkerName);

		FString arg = FString::Printf(TEXT("\"%s/\" %d %d %s %s %s"), *Batch->WorkingDirectory, InProcessId, 0, *Batch->InputFileName, *Batch->OutputFileName, *WorkerParameters);
		OneInputFile->AsObject()->SetStringField(TEXT("arg"), *arg);
		ShaderArray.Add(OneInputFile);
	}
	RootObj->SetArrayField(TEXT("shaders"), ShaderArray);

	FString JsonStr;
	const TSharedRef<TJsonWriter<>>& JsonWriter = TJsonWriterFactory<>::Create(&JsonStr);
	FJsonSerializer::Serialize(RootObj.ToSharedRef(), JsonWriter);

	t2 = time(NULL);
	if (t2 - t1 >= 1)
	{
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] over 1 seconds for GetShaderJson %d seconds to FJsonSerializer::Serialize"), t2 - t1);
	}
	t1 = t2;

	return JsonStr;
}

void FShaderCompileBKDistThreadRunnable::MoveFullToFlight(int32 maxsizetomove, TArray<FShaderBatch*>& flightarray, bool removefull)
{
	// move full batch jobs to flight
	int32 NumNewJobs = maxsizetomove < ShaderBatchesFull.Num() ? maxsizetomove : ShaderBatchesFull.Num();
	if (NumNewJobs > 0)
	{
		int32 DestJobIndexTemp = flightarray.AddZeroed(NumNewJobs);
		//int32 DestJobIndexFlight = ShaderBatchesInFlight.AddZeroed(NumNewJobs);
		for (int32 SrcJobIndex = 0; SrcJobIndex < NumNewJobs; SrcJobIndex++, DestJobIndexTemp++)
		{
			flightarray[DestJobIndexTemp] = ShaderBatchesFull[SrcJobIndex];
		}

		if (removefull)
		{
			ShaderBatchesFull.RemoveAt(0, NumNewJobs);
		}
	}

	UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] copy [%d] full batch"), ShaderBatchesFull.Num());
	/*
	if (ShaderBatching)
	{
		UE_LOG(LogShaderCompilers, Warning, TEXT("[bk_dist] should not reach here!!!"));

		// Check we've actually got jobs for this batch.
		check(ShaderBatching->NumJobs() > 0);

		// Make sure we've written out the worker files for any incomplete batches.
		ShaderBatching->WriteTransferFile();
		//ShaderBatchesInFlight.Add(ShaderBatching);
		flightarray.Add(ShaderBatching);

		if (removefull)
		{
			ShaderBatching = NULL;
		}
	}
	*/
}

int32 FShaderCompileBKDistThreadRunnable::ObtainJobsFromMgr(TArray<FShaderCommonCompileJobPtr>& OutJobs)
{
	int32 pendjobs = 0;
	for (int32 PriorityIndex = MaxPriorityIndex; PriorityIndex >= MinPriorityIndex; --PriorityIndex)
	{
		int32 NumJobs = Manager->AllJobs.GetPendingJobs(EShaderCompilerWorkerType::Distributed, (EShaderCompileJobPriority)PriorityIndex, 1, INT32_MAX, OutJobs);
		if (NumJobs > 0)
		{
			UE_LOG(LogShaderCompilers, Display, TEXT("Started %d 'bk_dist' shader compile jobs with '%s' priority"),
				NumJobs,
				ShaderCompileJobPriorityToString((EShaderCompileJobPriority)PriorityIndex));
		}
		pendjobs += NumJobs;
	}

	return pendjobs;
}

void FShaderCompileBKDistThreadRunnable::ObtainJobs()
{
	// UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] ObtainJobs in..."));

	uint64 t1 = time(NULL);
	uint64 t2 = t1;

	// Try to prepare more shader jobs (even if a build is in flight).
	TArray<FShaderCommonCompileJobPtr> JobQueue;
	{
		// Enter the critical section so we can access the input and output queues
		FScopeLock Lock(&Manager->CompileQueueSection);

		t2 = time(NULL);
		if (t2 - t1 >= 1)
		{
			UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] over 1 seconds for ObtainJobs %d seconds to wait lock"), t2 - t1);
		}
		t1 = t2;

		// Grab as many jobs from the job queue as we can.
		int32 NumNewJobs = Manager->AllJobs.GetNumPendingJobs();
		if (NumNewJobs > 0)
		{
#if BK_DIST_OPT_FEW_SHADER
			// if less than cpu core and no bk_dist jobs, do not launch bk_dist
			if (NumNewJobs < FPlatformMisc::NumberOfCores() && !HasJobs())
			{
				return;
			}
#endif
			ObtainJobsFromMgr(JobQueue);
		}

		t2 = time(NULL);
		if (t2 - t1 >= 1)
		{
			UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] over 1 seconds for ObtainJobs %d seconds to copy jobs from main thread"), t2 - t1);
		}
		t1 = t2;
	}

	if (JobQueue.Num() > 0)
	{
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] copied [%d] shader compile jobs from editor"), JobQueue.Num());

		// We have new jobs in the queue.
		// Group the jobs into batches and create the worker input files.
		for (int32 JobIndex = 0; JobIndex < JobQueue.Num(); JobIndex++)
		{
			if (!ShaderBatching)
			{
				BatchIndex++;
				if (BatchIndex < 0)
				{
					BatchIndex = 0;
					DirectoryIndex++;
				}

				// There are no more incomplete shader batches available.
				// Create another one...
				ShaderBatching = new FShaderBatch(
					WorkingDirectory,
					BKDistShaderCompiling::BKDist_InputFileSuffix,
					BKDistShaderCompiling::BKDist_OutputFileSuffix,
					Guid,
					DirectoryIndex,
					BatchIndex);
			}

			ShaderBatching->AddJob(JobQueue[JobIndex]);

			// If the batch is now full...
			int32 BatchMaxNum = FMath::Min<int32>(BKDistConsoleVariables::BatchSize,
				1 + JobQueue.Num() / (Manager->bCompilingDuringGame ?
					Manager->NumShaderCompilingThreadsDuringGame : Manager->NumShaderCompilingThreads));
			if (ShaderBatching->NumJobs() >= BatchMaxNum)
			{
				ShaderBatching->WriteTransferFile();

				// Move the batch to the full list.
				ShaderBatchesFull.Add(ShaderBatching);
				ShaderBatching = NULL;
			}
		}
		// append to full array anyway
		if (ShaderBatching)
		{
			ShaderBatching->WriteTransferFile();

			// Move the batch to the full list.
			ShaderBatchesFull.Add(ShaderBatching);
			ShaderBatching = NULL;
		}

		t2 = time(NULL);
		if (t2 - t1 >= 1)
		{
			UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] over 1 seconds for ObtainJobs %d seconds to copy to full array"), t2 - t1);
		}
		t1 = t2;

		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] get total [%d] full batch"), ShaderBatchesFull.Num());

		// fresh bk-shader-tool status
		CheckBKTool();
	}

	// send jobs after launched bk-shader-tool
	if (BKShaderAvailable && (ShaderBatching || ShaderBatchesFull.Num() > 0))
	{
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] ready generate json with [%d] full batch"), ShaderBatchesFull.Num());

		// copy to temp flight array
		TArray<FShaderBatch*> TempShaderBatchesInFlight;
		MoveFullToFlight(BKDistConsoleVariables::BatchGroupSize, TempShaderBatchesInFlight, false);
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] get total [%d] temp flight batch"), TempShaderBatchesInFlight.Num());

		t2 = time(NULL);
		if (t2 - t1 >= 1)
		{
			UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] over 1 seconds for ObtainJobs %d seconds to copy to ShaderBatchesInFlight"), t2 - t1);
		}
		t1 = t2;

		// generate actions json file by TempShaderBatchesInFlight
		FString jsonstr = GetShaderJson(Manager->ShaderCompileWorkerName, Manager->ProcessId, TempShaderBatchesInFlight);
		if (jsonstr.IsEmpty())
		{
			UE_LOG(LogShaderCompilers, Fatal, TEXT("[bk_dist] failed to get shader json"));
		}
		int ret = SendShaders(jsonstr);
		if (ret == 0)
		{
			// copy to real flight array
			MoveFullToFlight(BKDistConsoleVariables::BatchGroupSize, ShaderBatchesInFlight, true);
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] sent [%d] tasks to bk-shader-tool"), TempShaderBatchesInFlight.Num());
		}

		t2 = time(NULL);
		if (t2 - t1 >= 1)
		{
			UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] over 1 seconds for ObtainJobs %d seconds to generate shader json files"), t2 - t1);
		}
		t1 = t2;
	}
}

//bool FShaderCompileBKDistThreadRunnable::IsBKShaderAvailable()
//{
//	return FShaderCompileBKDistThreadRunnable::BKShaderAvailable.GetValue() == 1;
//}

bool FShaderCompileBKDistThreadRunnable::HasJobs()
{
	return !(ShaderBatching == NULL && ShaderBatchesFull.Num() == 0 && ShaderBatchesInFlight.Num() == 0);
}

void FShaderCompileBKDistThreadRunnable::LaunchBKProcessIfNeeded()
{
	// do not launch if no tasks
	if (!HasJobs())
	{
		return;
	}

	if (BKShaderAvailable)
	{
		return;
	}

	if (InLaunching)
	{
		return;
	}

	// launched by others, do nothing here
	if (BKShaderAvailable)
	{
		return;
	}

	if (BuildProcessHandle.IsValid())
	{
		FPlatformProcess::CloseProc(BuildProcessHandle);
	}

	LaunchTime = FDateTime::UtcNow();
	LastLaunchTime = time(NULL);
	BKShaderToolID = 0;
	InLaunching = true;

#if BK_DIST_DYNAMIC_PORT
	FString args = FString::Printf(TEXT("--tool_dir \"%s\" --port %d --process_info_file \"%s\""), *BKDistShaderCompiling::bkToolDir, BKDistShaderCompiling::BKShaderPort, *BKDistShaderCompiling::BKDist_ProcessInfo);
#else
	FString args = FString::Printf(TEXT("--tool_dir \"%s\" --port %d"), *BKDistShaderCompiling::bkToolDir, BKDistShaderCompiling::BKShaderPort);
#endif
	// Kick off the BKDist process...
	UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] ready launch bk tool with cmd [%s %s]"), *BKDistShaderCompiling::bkToolShader, *args);
	BuildProcessHandle = FPlatformProcess::CreateProc(*BKDistShaderCompiling::bkToolShader, *args, true, false, true, &BuildProcessID, 0, nullptr, nullptr);
	if (!BuildProcessHandle.IsValid())
	{
		UE_LOG(LogShaderCompilers, Fatal, TEXT("[bk_dist] Failed to launch '%s' during shader compilation."), *BKDistShaderCompiling::bkToolShader);
	}
	else
	{
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] luanched %s %s"), *BKDistShaderCompiling::bkToolShader, *args);
	}
}

// to ensure BK process is working right
void FShaderCompileBKDistThreadRunnable::CheckBKProcessStatus()
{
	// do not check if no tasks
	if (!HasJobs())
	{
		return;
	}

	// check per 2 seconds if avaiable
	int chkinterval = 2;
	if (BKShaderAvailable)
	{
		chkinterval = 1;
	}
	uint64 now = time(NULL);
	if (now - LastCheckTime < chkinterval)
	{
		return;
	}
	LastCheckTime = now;

	CheckBKTool();

	// bk-shader-tool never succeed after wait 300 seconds, just quit with fatal
	if (InLaunching && !BKShaderAvailable && LastLaunchTime > 0 && now - LastLaunchTime > 300)
	{
		UE_LOG(LogShaderCompilers, Fatal, TEXT("[bk_dist] failed to launch bk-shader-tool after wait long time."));

		return;
	}
}

// Reclaim jobs from the workers which did not succeed (if any).
void FShaderCompileBKDistThreadRunnable::ReclaimJobs()
{
	for (FShaderBatch* Batch : ShaderBatchesInFlight)
	{
		// Delete any output/success files, but keep the input file so we don't have to write it out again.
		Batch->CleanUpFiles(true);

		// We can't add any jobs to a shader batch which has already been written out to disk,
		// so put the batch back into the full batches list, even if the batch isn't full.
		ShaderBatchesFull.Add(Batch);

		// Reset the batch/directory indices and move the input file to the correct place.
		BatchIndex++;
		if (BatchIndex < 0)
		{
			BatchIndex = 0;
			DirectoryIndex++;
		}
		FString OldInputFilename = Batch->InputFileNameAndPath;
		Batch->SetIndices(DirectoryIndex, BatchIndex);
		BKDistShaderCompiling::MoveFileHelper(Batch->InputFileNameAndPath, OldInputFilename);
	}
	ShaderBatchesInFlight.Empty(BKDistConsoleVariables::BatchGroupSize);
	UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] after reclaim jobs, get total [%d] full batch"), ShaderBatchesFull.Num());
}

int FShaderCompileBKDistThreadRunnable::SendMessage(FString IPStr, int32 port, FString message, FString url, FString* outret)
{
	FIPv4Address ip;
	FIPv4Address::Parse(IPStr, ip);

	//
	TSharedPtr<FInternetAddr> addr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
	addr->SetIp(ip.Value);
	addr->SetPort(port);

	//
	FSocket* s = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateSocket(NAME_Stream, TEXT("default"), false);
	// connect
	if (!s->Connect(*addr))
	{
		// UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] Connect failed!"));
#ifndef PLATFORM_MAC
		s->Close();
#endif
		delete s;
		s = NULL;
		return 1;
	}
	UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] succeed to connect to [%s:%d]"), *IPStr, port);

	// send
	TArray<uint8>* httpbuf = FormatHttpRequest(port, message, url);
	int32 sent = 0;
	if (!s->Send((uint8*)httpbuf->GetData(), httpbuf->Num(), sent))
	{
		delete httpbuf;
		httpbuf = NULL;
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] Send Failed!"));
#ifndef PLATFORM_MAC
		s->Close();
#endif
		delete s;
		s = NULL;
		return 2;
	}
	delete httpbuf;
	httpbuf = NULL;
	UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] succeed to send data to [%s:%d]"), *IPStr, port);

	// receive
	TArray<uint8> buff;
	buff.Init(0, 1024u);
	int32 BytesRead = 0;
	if (!s->Recv(buff.GetData(), buff.Num(), BytesRead))
	{
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] failed to recv data from [%s:%d]"), *IPStr, port);
#ifndef PLATFORM_MAC
		s->Close();
#endif
		delete s;
		s = NULL;
		return 3;
	}
	if (BytesRead > 0)
	{
		const FString str = FString(ANSI_TO_TCHAR(reinterpret_cast<const char*>(buff.GetData())));
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] succeed to recv data[%s] from [%s:%d]"), *str, *IPStr, port);
		*outret = str;
	}

	//
#ifndef PLATFORM_MAC
	s->Close();
#endif
	delete s;
	s = NULL;
	return 0;
}

TArray<uint8>* FShaderCompileBKDistThreadRunnable::FormatHttpRequest(int32 port, FString message, FString url)
{
	// fill Content-Length with utf8 length
	FTCHARToUTF8 Converted(*message);

	// FString httphead = FString::Printf(TEXT("POST %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Length: %d\r\n\r\n"), *url, BKDistShaderCompiling::BKShaderPort, Converted.Length());
	FString httphead = FString::Printf(TEXT("POST %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Length: %d\r\n\r\n"), *url, port, Converted.Length());
	TArray<uint8>* buff = new TArray<uint8>();
	buff->SetNum(httphead.Len() + Converted.Length());
	memcpy(buff->GetData(), TCHAR_TO_ANSI(*httphead), httphead.Len());
	memcpy(buff->GetData() + httphead.Len(), (uint8*)Converted.Get(), Converted.Length());

	return buff;
}

bool FShaderCompileBKDistThreadRunnable::IsBKShaderToolPIDChanged(FString& response)
{
	if (response.IsEmpty())
	{
		return false;
	}

	TSharedPtr<FJsonObject> Object;
	TSharedRef<TJsonReader<> > Reader = TJsonReaderFactory<>::Create(response);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] failed to Deserialize http response [%s]"), *response);
		return false;
	}

	const TSharedPtr<FJsonObject>* data;
	if (Object->TryGetObjectField(TEXT("data"), data))
	{
		int32 pid;
		if ((*data)->TryGetNumberField(TEXT("pid"), pid))
		{
			if (pid > 0)
			{
				if (BKShaderToolID == 0)
				{
					BKShaderToolID = pid;
					UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] got init bk-shader-tool process id [%d]"), BKShaderToolID);
					return false;
				}
				else
				{
					if (BKShaderToolID != pid)
					{
						UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] bk-shader-tool process id changed from [%d] to [%d]"), BKShaderToolID, pid);
						BKShaderToolID = pid;
						return true;
					}
				}
			}
		}
	}

	return false;
}

int FShaderCompileBKDistThreadRunnable::GetRetCode(FString& response)
{
	if (response.IsEmpty())
	{
		return 0;
	}

	TSharedPtr<FJsonObject> Object;
	TSharedRef<TJsonReader<> > Reader = TJsonReaderFactory<>::Create(response);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] failed to Deserialize http response [%s]"), *response);
		return 0;
	}

	int32 retcode = 0;
	if (Object->TryGetNumberField(TEXT("code"), retcode))
	{
		return retcode;
	}

	return retcode;
}

FString FShaderCompileBKDistThreadRunnable::GetHttpJson(FString& response)
{
	FString left;
	FString right;
	if (response.Split("\r\n\r\n", &left, &right))
	{
		return right;
	}

	return TEXT("");
}

#if BK_DIST_DYNAMIC_PORT
int32 FShaderCompileBKDistThreadRunnable::getPortFromProcessInfo()
{
	int32 port = -1;
	FString JsonString;
	if (FFileHelper::LoadFileToString(JsonString, *BKDistShaderCompiling::BKDist_ProcessInfo))
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<> > Reader = TJsonReaderFactory<>::Create(JsonString);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] Deserialize process info [%s] failed"), *JsonString);
			return port;
		}

		if (Object->TryGetNumberField(TEXT("listen_port"), port))
		{
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] got port [%d] from process info file %s"), port, *BKDistShaderCompiling::BKDist_ProcessInfo);
			return port;
		}

		return port;
	}

	return port;
}

void FShaderCompileBKDistThreadRunnable::CheckBKTool()
{
	int32 port = getPortFromProcessInfo();
	if (port <= 0)
	{
		return;
	}

	FString outret;
	FString httpjson;
	int ret = SendMessage(TEXT("127.0.0.1"), port, TEXT("{}"), TEXT("/api/v1/available"), &outret);
	switch (ret)
	{
	case 0:
		// process is running, but it is maybe a new process!!!
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] bk-shader-tool is running with response[%s]"), *outret);
		httpjson = GetHttpJson(outret);
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] got http json response[%s]"), *httpjson);
		// if process of bk-shader-tool changed, enusre all tasks re-send to the new bk-shader-tool
		if (IsBKShaderToolPIDChanged(httpjson))
		{
			ReclaimJobs();
		}
		BKDistShaderCompiling::BKShaderDynamicPort = port;
		BKCheckFailCounter = 0;
		BKShaderAvailable = true;
		InLaunching = false;
		break;
	case 1:
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] failed to connect to bk-shader-tool, waiting for bk-shader-tool launched"));
		break;
	case 2:
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] failed to send data to bk-shader-tool"));
		break;
	default:
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] unknown ret code %d when check bk-shader-tool"), ret);
	}

	if (ret != 0)
	{
		// call ReclaimJobs() if changed to unavailble
		if (BKShaderAvailable)
		{
			ReclaimJobs();
		}

		++BKCheckFailCounter;
		BKShaderAvailable = false;
	}
}
#else
void FShaderCompileBKDistThreadRunnable::CheckBKTool()
{
	FString outret;
	FString httpjson;
	int ret = SendMessage(TEXT("127.0.0.1"), BKDistShaderCompiling::BKShaderPort, TEXT("{}"), TEXT("/api/v1/available"), &outret);
	switch (ret)
	{
	case 0:
		// process is running, but it is maybe a new process!!!
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] bk-shader-tool is running with response[%s]"), *outret);
		httpjson = GetHttpJson(outret);
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] got http json response[%s]"), *httpjson);
		// if process of bk-shader-tool changed, enusre all tasks re-send to the new bk-shader-tool
		if (IsBKShaderToolPIDChanged(httpjson))
		{
			ReclaimJobs();
		}
		BKCheckFailCounter = 0;
		BKShaderAvailable = true;
		InLaunching = false;
		break;
	case 1:
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] failed to connect to bk-shader-tool, waiting for bk-shader-tool launched"));
		break;
	case 2:
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] failed to send data to bk-shader-tool"));
		break;
	default:
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] unknown ret code %d when check bk-shader-tool"), ret);
	}

	if (ret != 0)
	{
		// call ReclaimJobs() if changed to unavailble
		if (BKShaderAvailable)
		{
			ReclaimJobs();
		}

		++BKCheckFailCounter;
		BKShaderAvailable = false;
	}
}
#endif

int FShaderCompileBKDistThreadRunnable::SendShaders(FString& OutputJson)
{
	UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] ready send json to bk shader tool"));
	FString outret;
#if BK_DIST_DYNAMIC_PORT
	int ret = SendMessage(TEXT("127.0.0.1"), BKDistShaderCompiling::BKShaderDynamicPort, OutputJson, TEXT("/api/v1/shaders"), &outret);
#else
	int ret = SendMessage(TEXT("127.0.0.1"), BKDistShaderCompiling::BKShaderPort, OutputJson, TEXT("/api/v1/shaders"), &outret);
#endif
	if (ret == 0)
	{
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] succeed to send json to bk shader tool with http response [%s]"), *outret);
		// check json content, if error, return fail
		FString httpjson = GetHttpJson(outret);
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] got http json response[%s]"), *httpjson);
		int retcode = GetRetCode(httpjson);
		if (retcode != 0)
		{
			UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] failed to send json to bk shader tool with json retcode %d"), retcode);
		}
		return retcode;
	}
	else
	{
		UE_LOG(LogShaderCompilers, Display, TEXT("[bk_dist] failed to send json to bk shader tool with ret %d"), ret);
	}

	return ret;
}

int32 FShaderCompileBKDistThreadRunnable::CompilingLoop()
{
	uint64 t1 = time(NULL);

	// obtain jobs as possible
	ObtainJobs();
	uint64 t2 = time(NULL);
	if (t2 - t1 >= 1)
	{
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] over 1 seconds for ObtainJobs %d seconds"), t2 - t1);
	}
	t1 = t2;

	LaunchBKProcessIfNeeded();
	t2 = time(NULL);
	if (t2 - t1 >= 1)
	{
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] over 1 seconds for LaunchBKProcessIfNeeded %d seconds"), t2 - t1);
	}
	t1 = t2;

	// gather result
	GatherResultsFromBKDist();
	t2 = time(NULL);
	if (t2 - t1 >= 1)
	{
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] over 1 seconds for GatherResultsFromBKDist %d seconds"), t2 - t1);
	}
	t1 = t2;

	// clean completed jobs
	CleanCompleted();
	t2 = time(NULL);
	if (t2 - t1 >= 1)
	{
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] over 1 seconds for CleanCompleted %d seconds"), t2 - t1);
	}
	t1 = t2;

	// check bk process status
	CheckBKProcessStatus();
	t2 = time(NULL);
	if (t2 - t1 >= 1)
	{
		UE_LOG(LogShaderCompilers, Verbose, TEXT("[bk_dist] over 1 seconds for CheckBKProcessStatus %d seconds"), t2 - t1);
	}
	t1 = t2;

	// 
	if (!HasJobs() && Manager->bAllowAsynchronousShaderCompiling)
	{
		// Yield while there's nothing to do
		// Yield for a short while to stop this thread continuously polling the disk.
		FPlatformProcess::Sleep(0.01f);
	}

	return HasJobs() ? 1 : 0;
}
// -- to support bk_dist accelerate 