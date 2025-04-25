// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using EpicGames.Core;
using Microsoft.Extensions.Logging;
using UnrealBuildBase;
using UnrealBuildTool.Artifacts;
// ++ to support bk_dist accelerate
using System.Text.RegularExpressions;
// --

namespace UnrealBuildTool
{

	/// <summary>
	/// This executor uses async Tasks to process the action graph
	/// </summary>
	class ParallelExecutor : ActionExecutor
	{
		/// <summary>
		/// Maximum processor count for local execution. 
		/// </summary>
		[XmlConfigFile]
		[Obsolete("ParallelExecutor.MaxProcessorCount is deprecated. Please update xml to use BuildConfiguration.MaxParallelActions")]
#pragma warning disable 0169
		private static int MaxProcessorCount;
#pragma warning restore 0169

		/// <summary>
		/// Processor count multiplier for local execution. Can be below 1 to reserve CPU for other tasks.
		/// When using the local executor (not XGE), run a single action on each CPU core. Note that you can set this to a larger value
		/// to get slightly faster build times in many cases, but your computer's responsiveness during compiling may be much worse.
		/// This value is ignored if the CPU does not support hyper-threading.
		/// </summary>
		[XmlConfigFile]
		private static double ProcessorCountMultiplier = 1.0;

		/// <summary>
		/// Free memory per action in bytes, used to limit the number of parallel actions if the machine is memory starved.
		/// Set to 0 to disable free memory checking.
		/// </summary>
		[XmlConfigFile]
		private static double MemoryPerActionBytes = 1.5 * 1024 * 1024 * 1024;

		/// <summary>
		/// The priority to set for spawned processes.
		/// Valid Settings: Idle, BelowNormal, Normal, AboveNormal, High
		/// Default: BelowNormal or Normal for an Asymmetrical processor as BelowNormal can cause scheduling issues.
		/// </summary>
		[XmlConfigFile]
		protected static ProcessPriorityClass ProcessPriority = Utils.IsAsymmetricalProcessor() ? ProcessPriorityClass.Normal : ProcessPriorityClass.BelowNormal;

		/// <summary>
		/// When enabled, will stop compiling targets after a compile error occurs.
		/// </summary>
		[XmlConfigFile]
		private static bool bStopCompilationAfterErrors = false;

		/// <summary>
		/// Whether to show compilation times along with worst offenders or not.
		/// </summary>
		[XmlConfigFile]
		private static bool bShowCompilationTimes = Unreal.IsBuildMachine();

		/// <summary>
		/// Whether to show compilation times for each executed action
		/// </summary>
		[XmlConfigFile]
		private static bool bShowPerActionCompilationTimes = Unreal.IsBuildMachine();

		/// <summary>
		/// Whether to log command lines for actions being executed
		/// </summary>
		[XmlConfigFile]
		private static bool bLogActionCommandLines = false;

		/// <summary>
		/// Add target names for each action executed
		/// </summary>
		[XmlConfigFile]
		private static bool bPrintActionTargetNames = false;

		/// <summary>
		/// Whether to take into account the Action's weight when determining to do more work or not.
		/// </summary>
		[XmlConfigFile]
		protected static bool bUseActionWeights = false;

		/// <summary>
		/// Whether to show CPU utilization after the work is complete.
		/// </summary>
		[XmlConfigFile]
		protected static bool bShowCPUUtilization = Unreal.IsBuildMachine();

		/// <summary>
		/// Collapse non-error output lines
		/// </summary>
		private bool bCompactOutput = false;

		/// <summary>
		/// How many processes that will be executed in parallel
		/// </summary>
		public int NumParallelProcesses { get; private set; }

		private static readonly char[] LineEndingSplit = new char[] { '\n', '\r' };

		public static int GetDefaultNumParallelProcesses(int MaxLocalActions, bool bAllCores, ILogger Logger)
		{
			double MemoryPerActionBytesComputed = Math.Max(MemoryPerActionBytes, MemoryPerActionBytesOverride);
			if (MemoryPerActionBytesComputed > MemoryPerActionBytes)
			{
				Logger.LogInformation("Overriding MemoryPerAction with target-defined value of {Memory} bytes", MemoryPerActionBytesComputed / 1024 / 1024 / 1024);
			}

			return Utils.GetMaxActionsToExecuteInParallel(MaxLocalActions, bAllCores ? 1.0f : ProcessorCountMultiplier, bAllCores, Convert.ToInt64(MemoryPerActionBytesComputed));
		}

		/// <summary>
		/// Constructor
		/// </summary>
		/// <param name="MaxLocalActions">How many actions to execute in parallel</param>
		/// <param name="bAllCores">Consider logical cores when determining how many total cpu cores are available</param>
		/// <param name="bCompactOutput">Should output be written in a compact fashion</param>
		/// <param name="Logger">Logger for output</param>
		public ParallelExecutor(int MaxLocalActions, bool bAllCores, bool bCompactOutput, ILogger Logger)
			: base(Logger)
		{
			XmlConfig.ApplyTo(this);

			// Figure out how many processors to use
			NumParallelProcesses = GetDefaultNumParallelProcesses(MaxLocalActions, bAllCores, Logger);

			this.bCompactOutput = bCompactOutput;
		}

		/// <summary>
		/// Returns the name of this executor
		/// </summary>
		public override string Name => "Parallel";

		/// <summary>
		/// Checks whether the task executor can be used
		/// </summary>
		/// <returns>True if the task executor can be used</returns>
		public static bool IsAvailable()
		{
			return true;
		}

		/// <summary>
		/// Telemetry event for this executor
		/// </summary>
		protected TelemetryExecutorEvent? telemetryEvent;

		/// <inheritdoc/>
		public override TelemetryExecutorEvent? GetTelemetryEvent() => telemetryEvent;

		/// <summary>
		/// Create an action queue
		/// </summary>
		/// <param name="actionsToExecute">Actions to be executed</param>
		/// <param name="actionArtifactCache">Artifact cache</param>
		/// <param name="maxActionArtifactCacheTasks">Max artifact tasks that can execute in parallel</param>
		/// <param name="logger">Logging interface</param>
		/// <returns>Action queue</returns>
		public ImmediateActionQueue CreateActionQueue(IEnumerable<LinkedAction> actionsToExecute, IActionArtifactCache? actionArtifactCache, int maxActionArtifactCacheTasks, ILogger logger)
		{
			return new(actionsToExecute, actionArtifactCache, maxActionArtifactCacheTasks, "Compiling C++ source code...", x => WriteToolOutput(x), () => FlushToolOutput(), logger)
			{
				ShowCompilationTimes = bShowCompilationTimes,
				ShowCPUUtilization = bShowCPUUtilization,
				PrintActionTargetNames = bPrintActionTargetNames,
				LogActionCommandLines = bLogActionCommandLines,
				ShowPerActionCompilationTimes = bShowPerActionCompilationTimes,
				CompactOutput = bCompactOutput,
				StopCompilationAfterErrors = bStopCompilationAfterErrors,
			};
		}

		/// <inheritdoc/>
		public override async Task<bool> ExecuteActionsAsync(IEnumerable<LinkedAction> ActionsToExecute, ILogger Logger, IActionArtifactCache? actionArtifactCache)
		{
			if (!ActionsToExecute.Any())
			{
				return true;
			}
			
			// ++ to support bk_dist accelerate
			if (BKBuildToolChain.InitTools() && BKBuildToolChain.NeedAccelerateNoHook(ActionsToExecute) && BKBuildToolChain.ApplyResource(ActionsToExecute, Logger))
			{
				// Log.TraceInformation("[bk_dist] status: bk tools enabled at {0}", BKBuildToolChain.CurrentTime());
				Logger.LogInformation("[bk_dist] status: bk tools enabled at {time}", BKBuildToolChain.CurrentTime());

				if (BKBuildToolChain.BKExecuteAction(ActionsToExecute, Logger))
				{
					// Log.TraceInformation("[bk_dist] see details: http://bkbuildbooster-dashboard.oa.com/?id={0}", BKBuildToolChain.bkTaskID);
					Logger.LogInformation("[bk_dist] see details: http://bkbuildbooster-dashboard.oa.com/?id={taskid}", BKBuildToolChain.bkTaskID);
					return true;
				}
				else
				{
					// Log.TraceInformation("[bk_dist] status: failed to compile with bk tools");
					Logger.LogInformation("[bk_dist] status: failed to compile with bk tools");
					return false;
				}
			}
			else
			{
				// Log.TraceInformation("[bk_dist] status: bk tools disabled");
				Logger.LogInformation("[bk_dist] status: bk tools disabled");
			}
			// --
			
			DateTime startTimeUTC = DateTime.UtcNow;
			bool result;

			// The "useAutomaticQueue" should always be true unless manual queue is being tested
			bool useAutomaticQueue = true;
			if (useAutomaticQueue)
			{
				using ImmediateActionQueue queue = CreateActionQueue(ActionsToExecute, actionArtifactCache, NumParallelProcesses, Logger);
				int actionLimit = Math.Min(NumParallelProcesses, queue.TotalActions);
				queue.CreateAutomaticRunner(action => RunAction(queue, action), bUseActionWeights, actionLimit, NumParallelProcesses);
				queue.Start();
				queue.StartManyActions();
				result = await queue.RunTillDone();

				queue.GetActionResultCounts(out int totalActions, out int succeededActions, out int failedActions, out int cacheHitActions, out int cacheMissActions);
				telemetryEvent = new TelemetryExecutorEvent(Name, startTimeUTC, result, totalActions, succeededActions, failedActions, cacheHitActions, cacheMissActions, DateTime.UtcNow);
			}
			else
			{
				using ImmediateActionQueue queue = CreateActionQueue(ActionsToExecute, actionArtifactCache, NumParallelProcesses, Logger);
				int actionLimit = Math.Min(NumParallelProcesses, queue.TotalActions);
				ImmediateActionQueueRunner runner = queue.CreateManualRunner(action => RunAction(queue, action), bUseActionWeights, actionLimit, actionLimit);
				queue.Start();
				using Timer timer = new((_) => queue.StartManyActions(runner), null, 0, 500);
				queue.StartManyActions();
				result = await queue.RunTillDone();

				queue.GetActionResultCounts(out int totalActions, out int succeededActions, out int failedActions, out int cacheHitActions, out int cacheMissActions);
				telemetryEvent = new TelemetryExecutorEvent(Name, startTimeUTC, result, totalActions, succeededActions, failedActions, cacheHitActions, cacheMissActions, DateTime.UtcNow);
			}

			return result;
		}

		private static Func<Task>? RunAction(ImmediateActionQueue queue, LinkedAction action)
		{
			return async () =>
			{
				ExecuteResults results = await RunActionAsync(action, queue.ProcessGroup, queue.CancellationToken);
				queue.OnActionCompleted(action, results.ExitCode == 0, results);
			};
		}

		protected static async Task<ExecuteResults> RunActionAsync(LinkedAction Action, ManagedProcessGroup ProcessGroup, CancellationToken CancellationToken, string? AdditionalDescription = null)
		{
			CancellationToken.ThrowIfCancellationRequested();

			using ManagedProcess Process = new ManagedProcess(ProcessGroup, Action.CommandPath.FullName, Action.CommandArguments, Action.WorkingDirectory.FullName, null, null, ProcessPriority);

			using MemoryStream StdOutStream = new MemoryStream();
			await Process.CopyToAsync(StdOutStream, CancellationToken);

			CancellationToken.ThrowIfCancellationRequested();

			await Process.WaitForExitAsync(CancellationToken);

			List<string> LogLines = Console.OutputEncoding.GetString(StdOutStream.GetBuffer(), 0, Convert.ToInt32(StdOutStream.Length)).Split(LineEndingSplit, StringSplitOptions.RemoveEmptyEntries).ToList();
			int ExitCode = Process.ExitCode;
			TimeSpan ProcessorTime = Process.TotalProcessorTime;
			TimeSpan ExecutionTime = Process.ExitTime - Process.StartTime;
			return new ExecuteResults(LogLines, ExitCode, ExecutionTime, ProcessorTime, AdditionalDescription);
		}
	}

	/// <summary>
	/// Publicly visible static class that allows external access to the parallel executor config
	/// </summary>
	public static class ParallelExecutorConfiguration
	{
		/// <summary>
		/// Maximum number of processes that should be used for execution
		/// </summary>
		public static int GetMaxParallelProcesses(ILogger Logger) => ParallelExecutor.GetDefaultNumParallelProcesses(0, false, Logger);

		/// <summary>
		/// Maximum number of processes that should be used for execution
		/// </summary>
		public static int GetMaxParallelProcesses(int MaxLocalActions, bool bAllCores, ILogger Logger) => ParallelExecutor.GetDefaultNumParallelProcesses(MaxLocalActions, bAllCores, Logger);
	}


	// ++ to support bk_dist accelerate
	static class BKBuildToolChain
	{
		static private string bktoolpath = "";
		
		static private string bkboostername = "";
		static private string bkboosterfullpath = "";

		static private string bkapplyscriptname = "";
		static private string bkapplyscriptfullpath = "";

		// static private string bkexecutorname = "";
		// static private string bkexecutorfullpath = "";

		static private string bkidleloopname = "";
		static private string bkidleloopfullpath = "";

		static private string bkubttoolname = "";
		static private string bkubttoolfullpath = "";

		static private int bkMaxProcess = 0;
		static public string bkTaskID = "";

		static private string bktoolchainfullpath = "";

		static private string bkubaactionname = "bk_uba_action_template.json";
		static private string bkubaactionfullpath = "";

		private enum BKDistCompiler
		{
			Compiler_Unknown,
			Compiler_CL,      // cl.exe | cl-filter.exe
			Compiler_Clang,     // clang | clang++
			Compiler_UbaAgent,     // UbaAgent.exe
			Compiler_PS5_Clang,     // prospero-clang.exe
			Compiler_ClangCL,     // clang-cl.exe
		}

		static private Dictionary<BKDistCompiler,bool> bkdistcompilers = new Dictionary<BKDistCompiler, bool>();
		static private Dictionary<BKDistCompiler,string> bkdistcommandpaths = new Dictionary<BKDistCompiler, string>();

		public static int GetMaxProcess()
		{
			return bkMaxProcess;
		}

		//public static string GetExecutor()
		//{
		//	return bkexecutorfullpath;
		//}

		private static string GetRandomString(int length, bool useNum, bool useLow, bool useUpp, bool useSpe, string custom)
		{
			// byte[] b = new byte[4];
			// new System.Security.Cryptography.RNGCryptoServiceProvider().GetBytes(b);
			//Random r = new(BitConverter.ToInt32(b, 0));
			Random r = new(Guid.NewGuid().GetHashCode());
			string s = "", str = custom;
			if (useNum == true) { str += "0123456789"; }
			if (useLow == true) { str += "abcdefghijklmnopqrstuvwxyz"; }
			if (useUpp == true) { str += "ABCDEFGHIJKLMNOPQRSTUVWXYZ"; }
			if (useSpe == true) { str += "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"; }
			for (int i = 0; i < length; i++)
			{
				s += str.Substring(r.Next(0, str.Length - 1), 1);
			}
			return s;
		}

		private static BKDistCompiler ParseActionCompiler(LinkedAction action)
		{
			BKDistCompiler compiler = BKDistCompiler.Compiler_Unknown;
			string compilerFileName = action.CommandPath.GetFileName();
			if (compilerFileName.Equals("cl.exe", StringComparison.OrdinalIgnoreCase))
			{
				compiler = BKDistCompiler.Compiler_CL;
				
				//bkdistcommandpath = action.CommandPath.FullName;
				bkdistcompilers[BKDistCompiler.Compiler_CL] = true;
				bkdistcommandpaths[BKDistCompiler.Compiler_CL] = action.CommandPath.FullName;
			}
			else if (compilerFileName.Equals("cl-filter.exe", StringComparison.OrdinalIgnoreCase))
			{
				compiler = BKDistCompiler.Compiler_CL;

				//bkdistcommandpath = GetClfilterCommandPath(action);
				bkdistcompilers[BKDistCompiler.Compiler_CL] = true;
				bkdistcommandpaths[BKDistCompiler.Compiler_CL] = GetClfilterCommandPath(action);
			}
			else if (compilerFileName.Equals("clang.exe", StringComparison.OrdinalIgnoreCase) ||
				compilerFileName.Equals("clang++.exe", StringComparison.OrdinalIgnoreCase) ||
				compilerFileName.Equals("clang", StringComparison.OrdinalIgnoreCase) ||
				compilerFileName.Equals("clang++", StringComparison.OrdinalIgnoreCase))
			{
				compiler = BKDistCompiler.Compiler_Clang;

				// bkdistcommandpath = action.CommandPath.FullName;
				bkdistcompilers[BKDistCompiler.Compiler_Clang] = true;
				bkdistcommandpaths[BKDistCompiler.Compiler_Clang] = action.CommandPath.FullName;
			}
			else if (compilerFileName.Equals("prospero-clang.exe", StringComparison.OrdinalIgnoreCase))
			{
				compiler = BKDistCompiler.Compiler_PS5_Clang;
				bkdistcompilers[BKDistCompiler.Compiler_PS5_Clang] = true;
				bkdistcommandpaths[BKDistCompiler.Compiler_PS5_Clang] = action.CommandPath.FullName;
			}
			else if (compilerFileName.Equals("clang-cl.exe", StringComparison.OrdinalIgnoreCase))
			{
				compiler = BKDistCompiler.Compiler_ClangCL;
				bkdistcompilers[BKDistCompiler.Compiler_ClangCL] = true;
				bkdistcommandpaths[BKDistCompiler.Compiler_ClangCL] = action.CommandPath.FullName;
			}
			else if (IsPlatformMac())
			{
				if (compilerFileName.Equals("env", StringComparison.OrdinalIgnoreCase) ||
					compilerFileName.Equals("arch", StringComparison.OrdinalIgnoreCase))
				{
					Console.WriteLine(string.Format("[bk_dist] debug: check mac special action: name:[{0}] cmd:[{1}] arg:[{2}]", compilerFileName, action.CommandPath.ToString(), action.CommandArguments));
					string[] args = action.CommandArguments.Split(new Char[] { ' ' });
					if (args.Length > 1)
					{
						if (args[0].EndsWith("clang++") || args[0].EndsWith("clang"))
						{
							compiler = BKDistCompiler.Compiler_Clang;

							//bkdistcommandpath = args[0];
							bkdistcompilers[BKDistCompiler.Compiler_Clang] = true;
							bkdistcommandpaths[BKDistCompiler.Compiler_Clang] = args[0];
						} else if (args[1].EndsWith("clang++") || args[1].EndsWith("clang"))
						{
							compiler = BKDistCompiler.Compiler_Clang;

							//bkdistcommandpath = args[1];
							bkdistcompilers[BKDistCompiler.Compiler_Clang] = true;
							bkdistcommandpaths[BKDistCompiler.Compiler_Clang] = args[1];
						}
					}
				}
			}

			return compiler;
		}

		private static string GetClfilterCommandPath(LinkedAction action)
		{
			//string[] actionargs = action.CommandArguments.Split(new Char[] { '"' });
			//Console.WriteLine(string.Format("[bk_dist] full arg:{0}", action.CommandArguments));
			string[] actionargs = Regex.Split(action.CommandArguments, " (?=(?:[^\"]*\"[^\"]*\")*(?![^\"]*\"))");
			foreach (string i in actionargs)
			{
				string temp = i;
				if (temp.StartsWith("\"") && temp.EndsWith("\""))
				{
					temp = temp.Substring(1, temp.Length - 2);
				}

				//Console.WriteLine(string.Format("[bk_dist] part arg:{0}", temp));
				if (temp.EndsWith("cl.exe") && !temp.StartsWith("-compiler"))
				{
					return temp;
				}
			}

			return "";
		}

		private static string GetVCVersionByCommandPath()
		{
			if (bkdistcommandpaths.ContainsKey(BKDistCompiler.Compiler_CL))
			{
				string bkdistcommandpath = bkdistcommandpaths[BKDistCompiler.Compiler_CL];
				if (bkdistcommandpath != "")
				{
					if (bkdistcommandpath.Contains("\\2015\\"))
					{
						return "2015";
					}
					else if (bkdistcommandpath.Contains("\\2017\\"))
					{
						return "2017";
					}
					else if (bkdistcommandpath.Contains("\\2019\\"))
					{
						return "2019";
					}
					else if (bkdistcommandpath.Contains("\\2022\\"))
					{
						return "2022";
					}
				}
			}
			return "";
		}

		private static string GetVCVersionByCommandArgs()
		{
			String[] arguments = Environment.GetCommandLineArgs();
			foreach (string Arg in arguments)
			{
				string lowercaseArg = Arg.ToLowerInvariant();
				if (lowercaseArg == "-2015")
				{
					return "2015";
				}
				else if (lowercaseArg == "-2017")
				{
					return "2017";
				}
				else if (lowercaseArg == "-2019")
				{
					return "2019";
				}
				else if (lowercaseArg == "-2022")
				{
					return "2022";
				}
			}
			return "";
		}

		private static WindowsCompiler GetWindowsCompiler()
		{
			WindowsCompiler winCompiler = WindowsCompiler.Default;
			string vsversion = GetVCVersionByCommandPath();
			if (vsversion == "")
			{
				vsversion = GetVCVersionByCommandArgs();
			}

			if (vsversion != "")
			{
				//if (vsversion == "2015")
				//{
				//	winCompiler = WindowsCompiler.VisualStudio2015;
				//}
				//else 
				//if (vsversion == "2017")
				//{
				//	winCompiler = WindowsCompiler.VisualStudio2017;
				//}
				//else 
				//if (vsversion == "2019")
				//{
				//	winCompiler = WindowsCompiler.VisualStudio2019;
				//}
				if (vsversion == "2022")
				{
					winCompiler = WindowsCompiler.VisualStudio2022;
				}
			}

			return winCompiler;
		}

		private static VCEnvironment? GetVCEnv(ILogger Logger)
		{
			VCEnvironment? vcEnv = null;

			WindowsCompiler winCompiler = GetWindowsCompiler();

			if (winCompiler == WindowsCompiler.Default)
				winCompiler = WindowsPlatform.GetDefaultCompiler(null, UnrealArch.X64, Logger);

			if (winCompiler == WindowsCompiler.Default)
			{
				Console.WriteLine(string.Format("[bk_dist] check compiler: not found windows compiler"));
				return vcEnv;
			}
			else
			{
				try
				{
					vcEnv = VCEnvironment.Create(winCompiler, winCompiler, UnrealTargetPlatform.Win64, UnrealArch.X64, null, null, null, null, false, false, Logger);
				}
				catch (Exception e)
				{
					Console.WriteLine(string.Format("[bk_dist] check compiler: failed to create windows compiler with error: {0}", e));
					return null;
				}
			}

			return vcEnv;
		}

		public static bool IsPlatformWindows()
		{
			if (BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Win64)
			{
				return true;
			}

			return false;
		}

		private static bool IsPlatformMac()
		{
			if (BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Mac)
			{
				return true;
			}

			return false;
		}

		private static bool IsPlatformLinux()
		{
			if (BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Linux
				|| BuildHostPlatform.Current.Platform == UnrealTargetPlatform.LinuxArm64)
			{
				return true;
			}

			return false;
		}

		public static bool InitTools()
		{
			bkboostername = "bk-booster.exe";
			bkidleloopname = "bk-idle-loop.exe";
			// bkexecutorname = "bk-dist-executor.exe";
			bkubttoolname = "bk-ubt-tool.exe";
			bkapplyscriptname = "bk_apply_ubt.bat";
			if (!IsPlatformWindows())
			{
				bkboostername = "bk-booster";
				bkidleloopname = "bk-idle-loop";
				// bkexecutorname = "bk-dist-executor";
				bkubttoolname = "bk-ubt-tool";
				bkapplyscriptname = "bk_apply_ubt.sh";
			}

			string fullinengine = "";
			string exename = bkboostername;
			if (IsPlatformWindows())
			{
				fullinengine = Path.Combine(UnrealBuildBase.Unreal.EngineDirectory.FullName, "Build", "BatchFiles", "bk_dist_tools", exename);
			}
			else if (IsPlatformMac())
			{
				fullinengine = Path.Combine(UnrealBuildBase.Unreal.EngineDirectory.FullName, "Build", "BatchFiles", "Mac", "bk_dist_tools", exename);
			}
			else if (IsPlatformLinux())
			{
				fullinengine = Path.Combine(UnrealBuildBase.Unreal.EngineDirectory.FullName, "Build", "BatchFiles", "Linux", "bk_dist_tools", exename);
			}

			if (File.Exists(fullinengine))
			{
				bktoolpath = Path.GetDirectoryName(fullinengine) ?? "";
			}

			if (bktoolpath == "")
			{
				string envpath = Environment.GetEnvironmentVariable("BK_DIST_TOOLS_PATH") ?? "";
				if (envpath != null && envpath != "")
				{
					var paths = new[] { Environment.CurrentDirectory }.Concat(envpath.Split(Path.PathSeparator));
					foreach (string p in paths)
					{
						string f = Path.Combine(p, exename);
						if (File.Exists(f))
						{
							bktoolpath = Path.GetDirectoryName(f) ?? "";
						}
					}
				}
			}

			if (bktoolpath != "")
			{
				bkboosterfullpath = Path.Combine(bktoolpath, bkboostername) ?? "";
				//bkexecutorfullpath = Path.Combine(bktoolpath, bkexecutorname);
				//if (!File.Exists(bkexecutorfullpath))
				//{
				//	Console.WriteLine(string.Format("[bk_dist] check tools: tools {0} not existed, nor in the path specified by env BK_DIST_TOOLS_PATH", bkexecutorfullpath));
				//	return false;
				//}

				bkidleloopfullpath = Path.Combine(bktoolpath, bkidleloopname);
				if (!File.Exists(bkidleloopfullpath))
				{
					Console.WriteLine(string.Format("[bk_dist] check tools: tools {0} not existed, nor in the path specified by env BK_DIST_TOOLS_PATH", bkidleloopfullpath));
					return false;
				}

				bkubttoolfullpath = Path.Combine(bktoolpath, bkubttoolname);
				if (IsPlatformWindows() && !File.Exists(bkubttoolfullpath))
				{
					Console.WriteLine(string.Format("[bk_dist] check tools: tools {0} not existed, nor in the path specified by env BK_DIST_TOOLS_PATH", bkubttoolfullpath));
					return false;
				}

				bkapplyscriptfullpath = Path.Combine(bktoolpath, bkapplyscriptname);
				if (!File.Exists(bkapplyscriptfullpath))
				{
					Console.WriteLine(string.Format("[bk_dist] check tools: tools {0} not existed, nor in the path specified by env BK_DIST_TOOLS_PATH", bkapplyscriptfullpath));
					return false;
				}

				return true;
			}

			Console.WriteLine(string.Format("[bk_dist] check tools: tools {0} not existed, nor in the path specified by env BK_DIST_TOOLS_PATH", fullinengine));
			return false;
		}

		private static string GetBKToolPath()
		{
			return bktoolpath;
		}

		private static string NewToolChainFile()
		{
			string filename = "bkdisttoolchain_" + GetRandomString(10, true, true, true, false, "") + ".json";
			string bktoolpath = GetBKToolPath();
			if (bktoolpath == "")
			{
				Console.WriteLine("[bk_dist] check tool chain: bk tool path is empty");
				return "";
			}
			else
			{
				string toolChainFile = Path.Combine(bktoolpath, filename);
				// Console.WriteLine("[bk_dist] succeed to generate tool chain file {0}", toolChainFile);
				return toolChainFile;
			}
		}

		private static string NewOutputJsonFile()
		{
			string filename = "bkoutputjsonenv_" + GetRandomString(10, true, true, true, false, "") + ".json";
			string bktoolpath = GetBKToolPath();
			if (bktoolpath == "")
			{
				Console.WriteLine("[bk_dist] check tools: bk tool path is empty");
				return "";
			}
			else
			{
				string ouputFile = Path.Combine(bktoolpath, filename);
				// Console.WriteLine("[bk_dist] succeed to generate output json file {0}", ouputFile);
				return ouputFile;
			}
		}

		private static string NewActionsJsonFile()
		{
			string filename = "bk_actions_" + GetRandomString(10, true, true, true, false, "") + ".json";
			string bktoolpath = GetBKToolPath();
			if (bktoolpath == "")
			{
				Console.WriteLine("[bk_dist] check tools: bk tool path is empty");
				return "";
			}
			else
			{
				string ouputFile = Path.Combine(bktoolpath, filename);
				// Console.WriteLine("[bk_dist] succeed to generate actions json file {0}", ouputFile);
				return ouputFile;
			}
		}

		private static bool ResolveEnvJson(string jsonfile)
		{
			try
			{
				string jsonString = File.ReadAllText(jsonfile);
				bool parsingkey = true;
				var key = new System.Text.StringBuilder();
				var value = new System.Text.StringBuilder();
				foreach (char c in jsonString)
				{
					switch (c) {
						case '{':
						case '\"':
							break;
						case ':':
							if (parsingkey)
							{
								parsingkey = false;
							}
							else
							{
								value.Append(c);
							}
							break;
						case ',':
						case '}':
							parsingkey = true;
							Environment.SetEnvironmentVariable(key.ToString(), value.ToString());
							Console.WriteLine("[bk_dist] set env: {0}={1}", key.ToString(), value.ToString());
							if (key.ToString() == "BK_DIST_UE4_MAX_PROCESS")
							{
								bkMaxProcess = int.Parse(value.ToString());
							}
							else if (key.ToString() == "BK_DIST_TASK_ID")
							{
								bkTaskID = value.ToString();
							}
							key.Clear();
							value.Clear();
							break;
						default:
							if (parsingkey)
							{
								key.Append(c);
							}
							else
							{
								value.Append(c);
							}
							break;
					}
				}
			}
			catch (ArgumentException)
			{
				return false;
			}

			return true;
		}

		public static bool ApplyResource(IEnumerable<LinkedAction> InputActions, ILogger Logger)
		{
			bkMaxProcess = 0;

			// new tool chain json file
			bktoolchainfullpath = NewToolChainFile();
			if (bktoolchainfullpath == "")
			{
				Console.WriteLine("[bk_dist] check resource: failed to new tool chain json file");
				return false;
			}

			if (!WriteToolChainFile(InputActions, bktoolchainfullpath, Logger))
			{
				Console.WriteLine("[bk_dist] check resource: failed to write tool chain json file:{0}", bktoolchainfullpath);
				File.Delete(bktoolchainfullpath);
				return false;
			}
			
			// new ouput json file
			string outputJsonFile = NewOutputJsonFile();
			if (outputJsonFile == "")
			{
				Console.WriteLine("[bk_dist] check resource: failed to generate tool chain json file");
				File.Delete(bktoolchainfullpath);
				return false;
			}

			string param = string.Format(" \"{0}\" \"{1}\" \"{2}\" \"{3}\"", bkboosterfullpath, outputJsonFile, bktoolchainfullpath, bkidleloopfullpath);
			Console.WriteLine(string.Format("[bk_dist] run cmd: {0} {1}", bkapplyscriptfullpath, param));

			ProcessStartInfo startInfo = new ProcessStartInfo(bkapplyscriptfullpath, param)
			{
				CreateNoWindow = true,
				// do not use shell
				UseShellExecute = false,
				// redirect output
				RedirectStandardOutput = true,
				RedirectStandardError = true
			};

			Process process = new Process
			{
				StartInfo = startInfo
			};
			System.Diagnostics.Stopwatch timer = System.Diagnostics.Stopwatch.StartNew();
			process.Start();

			int maxwait = 70 * 1000; // 70 seconds
			int spent = 0;
			while (spent < maxwait)
			{
				if (!File.Exists(outputJsonFile))
				{
					if (process.HasExited)
					{
						break;
					}
					Thread.Sleep(1000);
					spent += 1000;
					continue;
				}

				// parse json and set env
				if (!ResolveEnvJson(outputJsonFile))
				{
					timer.Stop();
					Console.WriteLine(string.Format("[bk_dist] check resource: failed to resovle output env json file {0} after {1} seconds", outputJsonFile, timer.Elapsed.TotalSeconds));
					File.Delete(bktoolchainfullpath);
					File.Delete(outputJsonFile);
					return false;
				}

				timer.Stop();
				
				// delete json file anyway
				//File.Delete(bktoolchainfullpath);
				File.Delete(outputJsonFile);

				return true;
			}

			timer.Stop();
			Console.WriteLine(string.Format("[bk_dist] check resource: failed to apply resource after {0} seconds", timer.Elapsed.TotalSeconds));
			// delete json file anyway
			File.Delete(bktoolchainfullpath);
			File.Delete(outputJsonFile);
			return false;
		}

		private static bool WriteToolChainFile(IEnumerable<LinkedAction> Actions, string OutFilePath, ILogger Logger)
		{
			FileStream fstream = new FileStream(OutFilePath, FileMode.Create, FileAccess.Write);
			if (fstream == null)
			{
				Console.WriteLine(string.Format("[bk_dist] check tool chain: failed to create file[{0}]", OutFilePath));
				return false;
			}

			if (!WriteToolChain(fstream, Logger))
			{
				Console.WriteLine(string.Format("[bk_dist] check tool chain: failed to write tool chain to file[{0}]", OutFilePath));

				fstream.Close();
				return false;
			}

			fstream.Close();
			return true;
		}

		private static bool HasEnoughtActionsToAccelerate(IEnumerable<LinkedAction> Actions)
		{
			int processor = Environment.ProcessorCount;
			int bkActionNum = 0;
			foreach (LinkedAction action in Actions)
			{
				BKDistCompiler compiler = ParseActionCompiler(action);
				if (compiler != BKDistCompiler.Compiler_Unknown)
				{
					//bkdistcompiler = compiler;
					bkActionNum++;
					if (bkActionNum > processor)
					{
						// Console.WriteLine(string.Format("[bk_dist] found {0} action at least to accelerate, over than cpu number {1}, ready accelerate", bkActionNum, processor));
						return true;
					}
				}
			}

			Console.WriteLine(string.Format("[bk_dist] check actions: only found {0} actions, less than cpu number {1}, do not accelerate", bkActionNum, processor));
			return false;
		}

		private static bool IsBKDistPlatformSupport()
		{
			//Console.WriteLine(string.Format("[bk_dist] IsBKDistPlatformSupport..."));

			if (!IsPlatformWindows() && !IsPlatformMac() && !IsPlatformLinux())
			{
				Console.WriteLine("[bk_dist] check platform: do not support platform[{0}] now", BuildHostPlatform.Current.Platform);
				return false;
			}

			return true;
		}

		private static bool IsSwitchFileExisted()
		{
			string toolpath = GetBKToolPath();
			if (toolpath == "")
			{
				Console.WriteLine("[bk_dist] check tools: tools path is empty");
				return false;
			}

			string switchfile = Path.Combine(toolpath, "bk_dist_enable_ubt.json");
			if (!File.Exists(switchfile))
			{
				Console.WriteLine("[bk_dist] check switch: switch file: {0} is not existed", switchfile);
				return false;
			}

			return true;
		}


		public static bool NeedAccelerateNoHook(IEnumerable<LinkedAction> Actions)
		{
			return IsBKDistPlatformSupport() && HasEnoughtActionsToAccelerate(Actions) && IsSwitchFileExisted();
		}


		private static void AddText(FileStream fstream, string StringToWrite)
		{
			byte[] Info = new System.Text.UTF8Encoding(true).GetBytes(StringToWrite);
			fstream.Write(Info, 0, Info.Length);
		}

		/*
		private static string GetRelativePath(string fullpath)
		{
			string pathWithoutDrive = fullpath;
			if (fullpath.Length > 3 && fullpath[1] == ':')
			{
				pathWithoutDrive = fullpath.Substring(2);
			}

			return string.Format("{{{{task_id}}}}{0}", Path.GetDirectoryName(pathWithoutDrive));
		}
		*/

		private static string? GetJsonPath(string? fullpath)
		{
			if (fullpath == null)
			{
				return "";
			}
			return fullpath.Replace('\\', '/');
		}

		private static bool WriteToolChain(FileStream fstream, ILogger Logger)
		{
			// start
			AddText(fstream, "{\n\t\"toolchains\": [\n\t\t");
			bool firstToolChain = true;
			if (bkdistcompilers.ContainsKey(BKDistCompiler.Compiler_CL))
			{
				string bkdistcommandpath = bkdistcommandpaths[BKDistCompiler.Compiler_CL];
				//VCEnvironment vcEnv = GetVCEnv();
				//if (vcEnv != null)
				{
					// start
					if (firstToolChain)
					{
						firstToolChain = false;
						AddText(fstream, "\t\t{\n");
					}
					else
					{
						AddText(fstream, ",\n\t\t{\n");
					}

					// tool
					AddText(fstream, string.Format("\t\t\t\"tool_key\": \"{0}\",\n", GetJsonPath(bkdistcommandpath)));
					AddText(fstream, string.Format("\t\t\t\"tool_name\": \"{0}\",\n", Path.GetFileName(bkdistcommandpath)));
					AddText(fstream, string.Format("\t\t\t\"tool_local_full_path\": \"{0}\",\n", GetJsonPath(bkdistcommandpath)));
					// AddText(fstream, string.Format("\t\t\t\"tool_remote_relative_path\": \"{0}\",\n", GetJsonPath(GetRelativePath(vcEnv.CompilerPath.FullName))));
					AddText(fstream, string.Format("\t\t\t\"tool_remote_relative_path\": \"{0}\",\n", GetJsonPath(Path.GetDirectoryName(bkdistcommandpath))));

					// files
					AddText(fstream, "\t\t\t\"files\": [\n");
					//DirectoryReference toolChainRoot = DirectoryReference.Combine(vcEnv.ToolChainDir, "bin", "HostX64", "x64");
					string directoryPath = Path.GetDirectoryName(bkdistcommandpath) ?? "";
					DirectoryReference toolChainRoot = new(directoryPath);
					// first we use 1033 (English language) ui dll for cl.exe
					string cluiDll = Path.Combine(toolChainRoot.FullName, "1033", "clui.dll");
					if (!File.Exists(cluiDll))
					{
						// if not found 1033, then we found it in all sub-folders, and always use the first one.
						string[] cluiDllFiles = Directory.GetFiles(toolChainRoot.FullName, "clui.dll", SearchOption.AllDirectories);
						if (cluiDllFiles.Length > 0)
						{
							cluiDll = cluiDllFiles[0];
						}
					}
					if (File.Exists(cluiDll))
					{
						AddText(fstream, "\t\t\t\t{\n");
						AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", GetJsonPath(cluiDll)));
						// AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(GetRelativePath(cluiDll))));
						AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(Path.GetDirectoryName(cluiDll))));
						AddText(fstream, "\t\t\t\t},\n");
					}

					string mspdbExe = Path.Combine(toolChainRoot.FullName, "mspdbsrv.exe");
					if (File.Exists(mspdbExe))
					{
						AddText(fstream, "\t\t\t\t{\n");
						AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", GetJsonPath(mspdbExe)));
						// AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(GetRelativePath(mspdbExe))));
						AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(Path.GetDirectoryName(mspdbExe))));
						AddText(fstream, "\t\t\t\t},\n");
					}

					string[] dllFiles = Directory.GetFiles(toolChainRoot.FullName, "*.dll", SearchOption.TopDirectoryOnly);
					for (int i = 0; i < dllFiles.Length; i++)
					{
						AddText(fstream, "\t\t\t\t{\n");
						string jsonpath = GetJsonPath(dllFiles[i]) ?? "";
						AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", jsonpath));
						// AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(GetRelativePath(jsonpath))));
						AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(Path.GetDirectoryName(jsonpath))));
						if (i != dllFiles.Length - 1)
						{
							AddText(fstream, "\t\t\t\t},\n");
						}
						else
						{
							AddText(fstream, "\t\t\t\t}\n");
						}
					}
					AddText(fstream, "\t\t\t]\n");
					AddText(fstream, "\t\t}");

					// to support link.exe and lib.exe
					// tool of link.exe
					string linkExe = Path.Combine(toolChainRoot.FullName, "link.exe");
					if (File.Exists(linkExe))
					{
						// start
						if (firstToolChain)
						{
							firstToolChain = false;
							AddText(fstream, "\t\t{\n");
						}
						else
						{
							AddText(fstream, ",\n\t\t{\n");
						}

						AddText(fstream, string.Format("\t\t\t\"tool_key\": \"{0}\",\n", GetJsonPath(linkExe)));
						AddText(fstream, string.Format("\t\t\t\"tool_name\": \"{0}\",\n", "link.exe"));
						AddText(fstream, string.Format("\t\t\t\"tool_local_full_path\": \"{0}\",\n", GetJsonPath(linkExe)));
						AddText(fstream, string.Format("\t\t\t\"tool_remote_relative_path\": \"{0}\",\n", GetJsonPath(Path.GetDirectoryName(linkExe))));

						AddText(fstream, "\t\t\t\"files\": [\n");
						if (!File.Exists(cluiDll))
						{
							string[] cluiDllFiles = Directory.GetFiles(toolChainRoot.FullName, "clui.dll", SearchOption.AllDirectories);
							if (cluiDllFiles.Length > 0)
							{
								cluiDll = cluiDllFiles[0];
							}
						}
						if (File.Exists(cluiDll))
						{
							AddText(fstream, "\t\t\t\t{\n");
							AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", GetJsonPath(cluiDll)));
							AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(Path.GetDirectoryName(cluiDll))));
							AddText(fstream, "\t\t\t\t},\n");
						}

						if (File.Exists(mspdbExe))
						{
							AddText(fstream, "\t\t\t\t{\n");
							AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", GetJsonPath(mspdbExe)));
							AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(Path.GetDirectoryName(mspdbExe))));
							AddText(fstream, "\t\t\t\t},\n");
						}

						string cvtresExe = Path.Combine(toolChainRoot.FullName, "cvtres.exe");
						if (File.Exists(cvtresExe))
						{
							AddText(fstream, "\t\t\t\t{\n");
							AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", GetJsonPath(cvtresExe)));
							AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(Path.GetDirectoryName(cvtresExe))));
							AddText(fstream, "\t\t\t\t},\n");
						}

						for (int i = 0; i < dllFiles.Length; i++)
						{
							AddText(fstream, "\t\t\t\t{\n");
							string jsonpath = GetJsonPath(dllFiles[i])??"";
							AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", jsonpath));
							AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(Path.GetDirectoryName(jsonpath))));
							if (i != dllFiles.Length - 1)
							{
								AddText(fstream, "\t\t\t\t},\n");
							}
							else
							{
								AddText(fstream, "\t\t\t\t}\n");
							}
						}

						AddText(fstream, "\t\t\t]\n");
						AddText(fstream, "\t\t}");
					}

					// tool of lib.exe
					string libExe = Path.Combine(toolChainRoot.FullName, "lib.exe");
					if (File.Exists(libExe))
					{
						// start
						if (firstToolChain)
						{
							firstToolChain = false;
							AddText(fstream, "\t\t{\n");
						}
						else
						{
							AddText(fstream, ",\n\t\t{\n");
						}

						AddText(fstream, string.Format("\t\t\t\"tool_key\": \"{0}\",\n", GetJsonPath(libExe)));
						AddText(fstream, string.Format("\t\t\t\"tool_name\": \"{0}\",\n", "lib.exe"));
						AddText(fstream, string.Format("\t\t\t\"tool_local_full_path\": \"{0}\",\n", GetJsonPath(libExe)));
						AddText(fstream, string.Format("\t\t\t\"tool_remote_relative_path\": \"{0}\",\n", GetJsonPath(Path.GetDirectoryName(libExe))));

						AddText(fstream, "\t\t\t\"files\": [\n");
						if (!File.Exists(cluiDll))
						{
							string[] cluiDllFiles = Directory.GetFiles(toolChainRoot.FullName, "clui.dll", SearchOption.AllDirectories);
							if (cluiDllFiles.Length > 0)
							{
								cluiDll = cluiDllFiles[0];
							}
						}
						if (File.Exists(cluiDll))
						{
							AddText(fstream, "\t\t\t\t{\n");
							AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", GetJsonPath(cluiDll)));
							AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(Path.GetDirectoryName(cluiDll))));
							AddText(fstream, "\t\t\t\t},\n");
						}

						if (File.Exists(mspdbExe))
						{
							AddText(fstream, "\t\t\t\t{\n");
							AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", GetJsonPath(mspdbExe)));
							AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(Path.GetDirectoryName(mspdbExe))));
							AddText(fstream, "\t\t\t\t},\n");
						}

						string cvtresExe = Path.Combine(toolChainRoot.FullName, "cvtres.exe");
						if (File.Exists(cvtresExe))
						{
							AddText(fstream, "\t\t\t\t{\n");
							AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", GetJsonPath(cvtresExe)));
							AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(Path.GetDirectoryName(cvtresExe))));
							AddText(fstream, "\t\t\t\t},\n");
						}

						if (File.Exists(linkExe))
						{
							AddText(fstream, "\t\t\t\t{\n");
							AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", GetJsonPath(linkExe)));
							AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(Path.GetDirectoryName(linkExe))));
							AddText(fstream, "\t\t\t\t},\n");
						}

						for (int i = 0; i < dllFiles.Length; i++)
						{
							AddText(fstream, "\t\t\t\t{\n");
							string jsonpath = GetJsonPath(dllFiles[i])??"";
							AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", jsonpath));
							AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(Path.GetDirectoryName(jsonpath))));
							if (i != dllFiles.Length - 1)
							{
								AddText(fstream, "\t\t\t\t},\n");
							}
							else
							{
								AddText(fstream, "\t\t\t\t}\n");
							}
						}

						AddText(fstream, "\t\t\t]\n");
						AddText(fstream, "\t\t}");
					}
				}
			}

			if (bkdistcompilers.ContainsKey(BKDistCompiler.Compiler_Clang))
			{
				string bkdistcommandpath = bkdistcommandpaths[BKDistCompiler.Compiler_Clang];
				if (!System.IO.File.Exists(bkdistcommandpath))
				{
					Console.WriteLine("Can't find clang executable file! Path: {0}.", bkdistcommandpath);
					return false;
				}

				// start
				if (firstToolChain)
				{
					firstToolChain = false;
					AddText(fstream, "\t\t{\n");
				}
				else
				{
					AddText(fstream, ",\n\t\t{\n");
				}

				// tool
				AddText(fstream, string.Format("\t\t\t\"tool_key\": \"{0}\",\n", GetJsonPath(bkdistcommandpath)));
				AddText(fstream, string.Format("\t\t\t\"tool_name\": \"{0}\",\n", Path.GetFileName(bkdistcommandpath)));
				AddText(fstream, string.Format("\t\t\t\"tool_local_full_path\": \"{0}\",\n", GetJsonPath(bkdistcommandpath)));
				// AddText(fstream, string.Format("\t\t\t\"tool_remote_relative_path\": \"{0}\",\n", GetJsonPath(GetRelativePath(bkdistcommandpath))));
				AddText(fstream, string.Format("\t\t\t\"tool_remote_relative_path\": \"{0}\",\n", GetJsonPath(Path.GetDirectoryName(bkdistcommandpath))));

				// files
				AddText(fstream, "\t\t\t\"files\": [\n");
				string toolChainRoot = Path.GetDirectoryName(bkdistcommandpath)??"";
				DirectoryInfo dirToolChain = new(toolChainRoot);
				string targetsuffix = "";
				if (IsPlatformWindows())
				{
					targetsuffix = "*.dll";
				}
				else if (IsPlatformMac())
				{
					targetsuffix = "*.dylib";
				}
				FileInfo[] dllFiles = dirToolChain.GetFiles(targetsuffix, SearchOption.TopDirectoryOnly);
				for (int i = 0; i < dllFiles.Length; i++)
				{
					AddText(fstream, "\t\t\t\t{\n");
					string? jsonpath = GetJsonPath(dllFiles[i].FullName);
					AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", jsonpath));
					// AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(GetRelativePath(jsonpath))));
					string directpath = Path.GetDirectoryName(jsonpath)?? "";
					AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(directpath)));
					if (i != dllFiles.Length - 1)
					{
						AddText(fstream, "\t\t\t\t},\n");
					}
					else
					{
						AddText(fstream, "\t\t\t\t}\n");
					}
				}
				AddText(fstream, "\t\t\t]\n");
				AddText(fstream, "\t\t}");
			}

			if (bkdistcompilers.ContainsKey(BKDistCompiler.Compiler_UbaAgent))
			{
				string bkdistcommandpath = bkdistcommandpaths[BKDistCompiler.Compiler_UbaAgent];
				if (!System.IO.File.Exists(bkdistcommandpath))
				{
					Console.WriteLine("Can't find clang executable file! Path: {0}.", bkdistcommandpath);
					return false;
				}

				// start
				if (firstToolChain)
				{
					firstToolChain = false;
					AddText(fstream, "\t\t{\n");
				}
				else
				{
					AddText(fstream, ",\n\t\t{\n");
				}

				// tool
				AddText(fstream, string.Format("\t\t\t\"tool_key\": \"{0}\",\n", GetJsonPath(bkdistcommandpath)));
				AddText(fstream, string.Format("\t\t\t\"tool_name\": \"{0}\",\n", Path.GetFileName(bkdistcommandpath)));
				AddText(fstream, string.Format("\t\t\t\"tool_local_full_path\": \"{0}\",\n", GetJsonPath(bkdistcommandpath)));
				AddText(fstream, string.Format("\t\t\t\"tool_remote_relative_path\": \"{0}\",\n", GetJsonPath(Path.GetDirectoryName(bkdistcommandpath))));

				// files
				AddText(fstream, "\t\t\t\"files\": [\n");
				string toolChainRoot = Path.GetDirectoryName(bkdistcommandpath) ?? "";
				DirectoryInfo dirToolChain = new(toolChainRoot);
				string targetsuffix = "";
				if (IsPlatformWindows())
				{
					targetsuffix = "*.dll";
				}
				else if (IsPlatformMac())
				{
					targetsuffix = "*.dylib";
				}
				FileInfo[] dllFiles = dirToolChain.GetFiles(targetsuffix, SearchOption.TopDirectoryOnly);
				for (int i = 0; i < dllFiles.Length; i++)
				{
					AddText(fstream, "\t\t\t\t{\n");
					string? jsonpath = GetJsonPath(dllFiles[i].FullName);
					AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", jsonpath));
					string directpath = Path.GetDirectoryName(jsonpath) ?? "";
					AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(directpath)));
					if (i != dllFiles.Length - 1)
					{
						AddText(fstream, "\t\t\t\t},\n");
					}
					else
					{
						AddText(fstream, "\t\t\t\t}\n");
					}
				}
				AddText(fstream, "\t\t\t]\n");
				AddText(fstream, "\t\t}");
			}
			
			if (bkdistcompilers.ContainsKey(BKDistCompiler.Compiler_PS5_Clang))
			{
				string bkdistcommandpath = bkdistcommandpaths[BKDistCompiler.Compiler_PS5_Clang];
				{
					// start
					if (firstToolChain)
					{
						firstToolChain = false;
						AddText(fstream, "\t\t{\n");
					}
					else
					{
						AddText(fstream, ",\n\t\t{\n");
					}

					// tool
					AddText(fstream, string.Format("\t\t\t\"tool_key\": \"{0}\",\n", GetJsonPath(bkdistcommandpath)));
					AddText(fstream, string.Format("\t\t\t\"tool_name\": \"{0}\",\n", Path.GetFileName(bkdistcommandpath)));
					AddText(fstream, string.Format("\t\t\t\"tool_local_full_path\": \"{0}\",\n", GetJsonPath(bkdistcommandpath)));
					AddText(fstream, string.Format("\t\t\t\"tool_remote_relative_path\": \"{0}\",\n", GetJsonPath(Path.GetDirectoryName(bkdistcommandpath))));

					// files
					AddText(fstream, "\t\t\t\"files\": [\n");
					string directoryPath = Path.GetDirectoryName(bkdistcommandpath) ?? "";
					DirectoryReference toolChainRoot = new(directoryPath);
					string[] dllFiles = Directory.GetFiles(toolChainRoot.FullName, "*.dll", SearchOption.TopDirectoryOnly);
					for (int i = 0; i < dllFiles.Length; i++)
					{
						AddText(fstream, "\t\t\t\t{\n");
						string jsonpath = GetJsonPath(dllFiles[i]) ?? "";
						AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", jsonpath));
						AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(Path.GetDirectoryName(jsonpath))));
						if (i != dllFiles.Length - 1)
						{
							AddText(fstream, "\t\t\t\t},\n");
						}
						else
						{
							AddText(fstream, "\t\t\t\t}\n");
						}
					}
					AddText(fstream, "\t\t\t]\n");
					AddText(fstream, "\t\t}");
				}
			}

			if (bkdistcompilers.ContainsKey(BKDistCompiler.Compiler_ClangCL))
			{
				string bkdistcommandpath = bkdistcommandpaths[BKDistCompiler.Compiler_ClangCL];
				{
					// start
					if (firstToolChain)
					{
						firstToolChain = false;
						AddText(fstream, "\t\t{\n");
					}
					else
					{
						AddText(fstream, ",\n\t\t{\n");
					}

					// tool
					AddText(fstream, string.Format("\t\t\t\"tool_key\": \"{0}\",\n", GetJsonPath(bkdistcommandpath)));
					AddText(fstream, string.Format("\t\t\t\"tool_name\": \"{0}\",\n", Path.GetFileName(bkdistcommandpath)));
					AddText(fstream, string.Format("\t\t\t\"tool_local_full_path\": \"{0}\",\n", GetJsonPath(bkdistcommandpath)));
					AddText(fstream, string.Format("\t\t\t\"tool_remote_relative_path\": \"{0}\",\n", GetJsonPath(Path.GetDirectoryName(bkdistcommandpath))));

					// files
					AddText(fstream, "\t\t\t\"files\": [\n");
					string directoryPath = Path.GetDirectoryName(bkdistcommandpath) ?? "";
					DirectoryReference toolChainRoot = new(directoryPath);
					string[] dllFiles = Directory.GetFiles(toolChainRoot.FullName, "*.dll", SearchOption.TopDirectoryOnly);
					for (int i = 0; i < dllFiles.Length; i++)
					{
						AddText(fstream, "\t\t\t\t{\n");
						string jsonpath = GetJsonPath(dllFiles[i]) ?? "";
						AddText(fstream, string.Format("\t\t\t\t\t\"local_full_path\": \"{0}\",\n", jsonpath));
						AddText(fstream, string.Format("\t\t\t\t\t\"remote_relative_path\": \"{0}\"\n", GetJsonPath(Path.GetDirectoryName(jsonpath))));
						if (i != dllFiles.Length - 1)
						{
							AddText(fstream, "\t\t\t\t},\n");
						}
						else
						{
							AddText(fstream, "\t\t\t\t}\n");
						}
					}
					AddText(fstream, "\t\t\t]\n");
					AddText(fstream, "\t\t}");
				}
			}

			// end
			AddText(fstream, "\n\t]\n}");

			return true;
		}

		public static int GetIndex(LinkedAction target, Dictionary<LinkedAction, int> tmpDictionary)
		{
			int value;
			if (tmpDictionary.TryGetValue(target, out value))
			{
				return value;
			}

			return -1;
		}

		public static string CurrentTime()
		{
			DateTime now = DateTime.Now;
			return now.ToString("yyyy-MM-dd HH:mm:ss");
		}

		public static bool WriteActionJson(IEnumerable<LinkedAction> sortedActions, string jsonfile, Dictionary<LinkedAction, int> tmpDictionary)
		{
			if (!string.IsNullOrEmpty(jsonfile))
			{
				// output actions to json file
				JsonWriter w = new JsonWriter(jsonfile);
				w.WriteObjectStart();
				w.WriteArrayStart("actions");

				int i = 0;
				foreach (LinkedAction item in sortedActions)
				{
					w.WriteObjectStart();

					w.WriteValue("index", i.ToString());

					if (IsPlatformMac() && item.CommandPath.FullName.EndsWith("UnrealBuildTool.exe"))
					{
						string mono = Path.Combine(UnrealBuildBase.Unreal.EngineDirectory.FullName, "Binaries", "ThirdParty", "Mono", "Mac", "bin", "mono");
						w.WriteValue("cmd", mono);
						string arg = string.Format("\"{0}\" {1}", item.CommandPath.FullName, item.CommandArguments);
						w.WriteValue("arg", arg);
					}
					else if (IsPlatformMac() && 
						(item.CommandPath.FullName.EndsWith("env") || item.CommandPath.FullName.EndsWith("arch")))
					{
						string[] args = item.CommandArguments.Split(new Char[] { ' ' });
						if (args.Length > 1 && 
							(args[0].EndsWith("clang++") || args[0].EndsWith("clang")))
						{
							w.WriteValue("cmd", args[0]);
							string restArg = string.Join(" ", args.Skip(1));
							w.WriteValue("arg", restArg);
						}
						else if (args.Length > 1 && 
							(args[1].EndsWith("clang++") || args[1].EndsWith("clang")))
						{
							w.WriteValue("cmd", args[1]);
							string restArg = string.Join(" ", args.Skip(2));
							w.WriteValue("arg", restArg);
						}
						else
						{
							w.WriteValue("cmd", item.CommandPath.FullName.ToString());
							if (item.CommandArguments.Contains("cp -f \"\""))
							{
								string arg = item.CommandArguments.Replace("\"\"", "\"");
								w.WriteValue("arg", arg);
							}
							else
							{
								w.WriteValue("arg", item.CommandArguments);
							}
						}
					}
					else if (IsPlatformLinux() && item.CommandPath.FullName.EndsWith("UnrealBuildTool.exe"))
					{
						string mono = Path.Combine(UnrealBuildBase.Unreal.EngineDirectory.FullName, "Binaries", "ThirdParty", "Mono", "Linux", "bin", "mono");
						w.WriteValue("cmd", mono);
						string arg = string.Format("\"{0}\" {1}", item.CommandPath.FullName, item.CommandArguments);
						w.WriteValue("arg", arg);
					}
					else
					{
						w.WriteValue("cmd", item.CommandPath.FullName.ToString());
						if (item.CommandArguments.Contains("cp -f \"\""))
						{
							string arg = item.CommandArguments.Replace("\"\"", "\"");
							w.WriteValue("arg", arg);
						}
						else
						{
							w.WriteValue("arg", item.CommandArguments);
						}
					}
					w.WriteValue("workdir", item.WorkingDirectory.ToString());

					// Console.WriteLine("[bk_dist] action[{0}] {1} {2}", i, sortedActions[i].CommandPath, sortedActions[i].CommandArguments);

					w.WriteArrayStart("dep");
					foreach (var a in item.PrerequisiteActions)
					{
						int DepIndex = GetIndex(a, tmpDictionary);
						// Console.WriteLine("[bk_dist] action[{0}] depend {1}", i, DepIndex);
						if (DepIndex >= 0)
						{
							w.WriteValue(DepIndex.ToString());
						}
					}
					w.WriteArrayEnd();

					w.WriteObjectEnd();

					i++;
				}

				w.WriteArrayEnd();
				w.WriteObjectEnd();

				w.Dispose();

				return true;
			}

			return false;
		}

		private static Dictionary<LinkedAction, int> getCache(IEnumerable<LinkedAction> Actions)
		{
			Dictionary<LinkedAction, int> tmpDictionary = new Dictionary<LinkedAction, int>();

			int index = 0;
			foreach (LinkedAction item in Actions)
			{
				tmpDictionary.Add(item, index++);
			}

			return tmpDictionary;
		}

		public static bool BKExecuteAction(IEnumerable<LinkedAction> Actions, ILogger Logger)
		{
			// Log.TraceInformation("[bk_dist] status: start sort actions at {0}", BKBuildToolChain.CurrentTime());
			Logger.LogInformation("[bk_dist] status: start sort actions at {time}", BKBuildToolChain.CurrentTime());

			Dictionary<LinkedAction, int> tmpDictionary = getCache(Actions);
			IEnumerable <LinkedAction>? sortedActions = SortActions(Actions, tmpDictionary, Logger);
			// Log.TraceInformation("[bk_dist] status: end sort actions at {0}", BKBuildToolChain.CurrentTime());
			Logger.LogInformation("[bk_dist] status: end sort actions at {time}", BKBuildToolChain.CurrentTime());
			if (sortedActions != null)
			{
				// Log.TraceInformation("[bk_dist] status: start write action file at {0}", BKBuildToolChain.CurrentTime());
				Logger.LogInformation("[bk_dist] status: start write action file at {time}", BKBuildToolChain.CurrentTime());
				string jsonfile = NewActionsJsonFile();
				if (!WriteActionJson(sortedActions, jsonfile, tmpDictionary))
				{
					File.Delete(bktoolchainfullpath);
					return false;
				}
				// Log.TraceInformation("[bk_dist] status: end write action file at {0}", BKBuildToolChain.CurrentTime());
				Logger.LogInformation("[bk_dist] status: end write action file at {time}", BKBuildToolChain.CurrentTime());

				// execute actions
				if (!ExecuteWithJsonFile(jsonfile, false))
				{
					Console.WriteLine(string.Format("[bk_dist] status: failed to run actions with json file: {0} --actions_json_file {1}", bkubttoolfullpath, jsonfile));
					File.Delete(jsonfile);
					File.Delete(bktoolchainfullpath);
					return false;
				}

				// delete json file anyway
				File.Delete(jsonfile);

				// Console.WriteLine(string.Format("[bk_dist] succeed to run actions with json file: {0} --actions_json_file {1}", bkubttoolfullpath, jsonfile));
				File.Delete(bktoolchainfullpath);
				return true;
			}

			File.Delete(bktoolchainfullpath);
			return false;
		}

		private static bool ExecuteWithJsonFile(string jsonfile, bool uba)
		{
			System.Diagnostics.Stopwatch timer = System.Diagnostics.Stopwatch.StartNew();

			string param = string.Format("--actions_json_file \"{0}\"", jsonfile);
			if (IsPlatformMac() || uba)
			{
				param = string.Format("--actions_json_file \"{0}\" --tool_chain_json_file \"{1}\"", jsonfile, bktoolchainfullpath);
			}
			
			Console.WriteLine(string.Format("[bk_dist] run cmd: {0} {1}", bkubttoolfullpath, param));
			ProcessStartInfo startInfo = new ProcessStartInfo(bkubttoolfullpath, param)
			{
				WorkingDirectory = Path.Combine(UnrealBuildBase.Unreal.EngineDirectory.MakeRelativeTo(DirectoryReference.GetCurrentDirectory()), "Source"),
				// CreateNoWindow = true,
				// do not use shell
				UseShellExecute = false,
				// redirect output
				RedirectStandardOutput = true,
				RedirectStandardError = true
			};

			try
			{
				Process process = new Process()
				{
					StartInfo = startInfo,
					EnableRaisingEvents = true
				};

				DataReceivedEventHandler OutputEventHandler = (Sender, Args) =>
				{
					if (Args.Data != null)
						Console.WriteLine(Args.Data);
				};

				process.OutputDataReceived += OutputEventHandler;
				process.ErrorDataReceived += OutputEventHandler;

				process.Start();

				process.BeginOutputReadLine();
				process.BeginErrorReadLine();

				process.WaitForExit();

				timer.Stop();
				Console.WriteLine(string.Format("[bk_dist] status: Execute actions in {0} seconds.", timer.Elapsed.TotalSeconds));
				return process.ExitCode == 0;
			}
			catch (Exception e)
			{
				Console.WriteLine("[bk_dist] check bk-ubt-tool: failed to launch bk-ubt-tool process. Is it in your path? " + e.ToString());
				return false;
			}
		}

		private static bool IsActionsSorted(IEnumerable<LinkedAction> InActions, Dictionary<LinkedAction, int> tmpDictionary)
		{
			int NumSortErrors = 0;
			int ActionIndex = 0;
			foreach (LinkedAction item in InActions)
			{
				foreach (var a in item.PrerequisiteActions)
				{
					// int DepIndex = GetIndex(InActions,a);
					int DepIndex = GetIndex(a, tmpDictionary);
					if (DepIndex > ActionIndex)
					{
						NumSortErrors++;
						return false;
					}
				}
				ActionIndex++;
			}

			return NumSortErrors == 0;
		}

		private static IEnumerable<LinkedAction> SortOnce(IEnumerable<LinkedAction> InActions, Dictionary<LinkedAction, int> tmpDictionary)
		{
			List<LinkedAction> myList = new List<LinkedAction>();
			var UsedActions = new HashSet<int>();
			//for (int ActionIndex = 0; ActionIndex < InActions.Count; ActionIndex++)
			int ActionIndex = 0;
			foreach (LinkedAction item in InActions)
			{
				if (UsedActions.Contains(ActionIndex))
				{
					continue;
				}

				// to ensure PrerequisiteActions will be inserted firstly
				foreach (var a in item.PrerequisiteActions)
				{
					// int DepIndex = GetIndex(InActions,a);
					int DepIndex = GetIndex(a, tmpDictionary);
					if (UsedActions.Contains(DepIndex))
					{
						continue;
					}
					myList.Add(a);
					UsedActions.Add(DepIndex);
				}

				myList.Add(item);
				UsedActions.Add(ActionIndex);

				ActionIndex++;
			}

			IEnumerable<LinkedAction> Actions = myList;
			return Actions;
		}

		private static IEnumerable<LinkedAction>? SortActions(IEnumerable<LinkedAction> InActions, Dictionary<LinkedAction, int> tmpDictionary, ILogger Logger)
		{
			// Log.TraceInformation("[bk_dist] status: start check whether actions sorted at {0}", BKBuildToolChain.CurrentTime());
			Logger.LogInformation("[bk_dist] status: start check whether actions sorted at {time}", BKBuildToolChain.CurrentTime());
			if (IsActionsSorted(InActions, tmpDictionary))
			{
				// Log.TraceInformation("[bk_dist] status: end check with actions sorted at {0}", BKBuildToolChain.CurrentTime());
				Logger.LogInformation("[bk_dist] status: end check with actions sorted at {time}", BKBuildToolChain.CurrentTime());
				return InActions;
			}
			// Log.TraceInformation("[bk_dist] status: end check with actions not sorted at {0}", BKBuildToolChain.CurrentTime());
			Logger.LogInformation("[bk_dist] status: end check with actions not sorted at {time}", BKBuildToolChain.CurrentTime());

			int maxtry = 3;
			IEnumerable<LinkedAction> InputActions = InActions;
			for (int i = 0; i < maxtry; i++)
			{
				// Log.TraceInformation("[bk_dist] status: sort actions for the {0} times at {1}", i,BKBuildToolChain.CurrentTime());
				Logger.LogInformation("[bk_dist] status: sort actions for the {index} times at {time}", i, BKBuildToolChain.CurrentTime());
				IEnumerable<LinkedAction> ResultActions = SortOnce(InputActions, tmpDictionary);
				if (IsActionsSorted(ResultActions, tmpDictionary))
				{
					return ResultActions;
				}

				InputActions = ResultActions;
			}

			Console.WriteLine("[bk_dist] check actions: actions can't be sorted after {0} try", maxtry);
			return null;
		}

		// to support uba agent
		private static bool checkUbaActionTemplate()
		{
			if (bktoolpath != "")
			{
				bkubaactionfullpath = Path.Combine(bktoolpath, bkubaactionname);
				if (!File.Exists(bkubaactionfullpath))
				{
					Console.WriteLine(string.Format("[bk_dist] check tools: actin template {0} not existed", bkubaactionfullpath));
					return false;
				}

				return true;
			}

			Console.WriteLine(string.Format("[bk_dist] check tools: template {0} not found", bkubaactionname));
			return false;
		}

		private static bool checkUbaAgentToolchain()
		{
			FileReference ubaagentPath = FileReference.Combine(Unreal.EngineDirectory, "Binaries", "Win64", "UnrealBuildAccelerator", "x64", "UbaAgent.exe");
			if (!FileReference.Exists(ubaagentPath))
			{
				Console.WriteLine("[bk_dist] quit launch tbs uba agents for {0} not existed", ubaagentPath);
				return false;
			}

			bkdistcompilers[BKDistCompiler.Compiler_UbaAgent] = true;
			bkdistcommandpaths[BKDistCompiler.Compiler_UbaAgent] = ubaagentPath.FullName;

			return true;
		}

		public static bool BKExecuteLaunchUbaAgent(ILogger Logger)
		{
			Logger.LogInformation("[bk_dist] status: start sort actions at {time}", BKBuildToolChain.CurrentTime());

			// execute actions
			if (!ExecuteWithJsonFile(bkubaactionfullpath, true))
			{
				Console.WriteLine(string.Format("[bk_dist] status: failed to run actions with json file: {0} --actions_json_file {1}", bkubttoolfullpath, bkubaactionfullpath));
				File.Delete(bktoolchainfullpath);
				return false;
			}

			File.Delete(bktoolchainfullpath);
			return true;
		}

		public static void launchUbaAgents(IEnumerable<LinkedAction> InputActions, ILogger Logger)
		{
			Console.WriteLine("[bk_dist] ready launch tbs uba agents");
			if (!InitTools() || !IsSwitchFileExisted())
			{
				Console.WriteLine("[bk_dist] status: quit launch tbs uba agents for failed to check bk tools and switch file");
				return;
			}

			// check and get action file
			if (!checkUbaActionTemplate())
			{
				Console.WriteLine("[bk_dist] status: quit launch tbs uba agents for failed to check uba action template");
				return;
			}

			// check and get uba agent tool chain
			if (!checkUbaAgentToolchain())
			{
				Console.WriteLine("[bk_dist] status: quit launch tbs uba agents for failed to check uba agent");
				return;
			}

			// apply resource 
			if (!ApplyResource(InputActions, Logger))
			{
				Console.WriteLine("[bk_dist] status: quit launch tbs uba agents for failed to apply");
				return;
			}

			// start bk-ubt-tool
			BKExecuteLaunchUbaAgent(Logger);
		}
	}
	// -- to support bk_dist accelerate
}
