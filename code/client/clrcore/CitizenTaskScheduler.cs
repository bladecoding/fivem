using CitizenFX.Core.Native;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace CitizenFX.Core
{
	class CitizenSynchronizationContext : SynchronizationContext
	{
		private static readonly List<Action> m_scheduledTasks = new List<Action>();

		public override void Post(SendOrPostCallback d, object state)
		{
			lock (m_scheduledTasks)
			{
				m_scheduledTasks.Add(() => d(state));
			}
		}

		public static void Tick()
		{
			Action[] tasks;

			lock (m_scheduledTasks)
			{
				tasks = m_scheduledTasks.ToArray();
				m_scheduledTasks.Clear();
			}

			foreach (var task in tasks)
			{
				try
				{
					task();
				}
				catch (Exception e)
				{
					Debug.WriteLine($"Exception during executing Post callback: {e}");
				}
			}
		}

		public override SynchronizationContext CreateCopy()
		{
			return this;
		}
	}

    class CitizenTaskScheduler : TaskScheduler
    {
		private static readonly object m_inTickTasksLock = new object();
		private List<Task> m_inTickTasks;

        private readonly List<Task> m_runningTasks = new List<Task>();

        protected CitizenTaskScheduler()
        {
            
        }

        [SecurityCritical]
        protected override void QueueTask(Task task)
        {
			if (m_inTickTasks != null)
			{
				lock (m_inTickTasksLock)
				{
					if (m_inTickTasks != null)
						m_inTickTasks.Add(task);
				}
			}

			lock (m_runningTasks)
			{
				m_runningTasks.Add(task);
			}
        }

        [SecurityCritical]
        protected override bool TryExecuteTaskInline(Task task, bool taskWasPreviouslyQueued)
        {
			if (!taskWasPreviouslyQueued)
            {
                return TryExecuteTask(task);
            }

            return false;
        }

        [SecurityCritical]
        protected override IEnumerable<Task> GetScheduledTasks()
        {
			lock (m_runningTasks)
			{
				return m_runningTasks.ToArray();
			}
        }

        public override int MaximumConcurrencyLevel => 1;

	    public void Tick()
        {
			Task[] tasks;

			lock (m_runningTasks)
			{
				tasks = m_runningTasks.ToArray();
			}

			// ticks should be reentrant (Tick might invoke TriggerEvent, e.g.)
			List<Task> lastInTickTasks;

			lock (m_inTickTasksLock)
			{
				lastInTickTasks = m_inTickTasks;

				m_inTickTasks = new List<Task>();
			}

			do
			{
				foreach (var task in tasks)
				{
					InvokeTryExecuteTask(task);

					if (task.Exception != null)
					{
						Debug.WriteLine("Exception thrown by a task: {0}", task.Exception.ToString());
					}

					if (task.IsCompleted || task.IsFaulted || task.IsCanceled)
					{
						lock (m_runningTasks)
						{
							m_runningTasks.Remove(task);
						}
					}
				}

				lock (m_inTickTasksLock)
				{
					tasks = m_inTickTasks.ToArray();
					m_inTickTasks.Clear();
				}
			} while (tasks.Length != 0);

			lock (m_inTickTasksLock)
			{
				m_inTickTasks = lastInTickTasks;
			}
        }

		Dictionary<string, List<ProfilerSample>> TaskProfiling = new Dictionary<string, List<ProfilerSample>>();
		static readonly FieldInfo TaskActionField = typeof( Task ).GetField( "m_action", BindingFlags.NonPublic | BindingFlags.Instance );
		static readonly object LogLock = new object();
		DateTime LastLog = DateTime.UtcNow;
		TimeSpan LogInterval = TimeSpan.FromSeconds( 5 );
		ulong LogCounter = 0;
		static long LogCount = 0;

		private string GetLogPath() {
			var name = AppDomain.CurrentDomain.FriendlyName;
			var root = Environment.GetFolderPath( Environment.SpecialFolder.UserProfile );
			return Path.Combine( root, "profiling", $"profiling_{name}.csv" );
		}

		[SecuritySafeCritical]
		private void EnsureParentDirExists( string path ) {
			var fi = new FileInfo( path );
			if( !fi.Directory.Exists )
				fi.Directory.Create();
		}

		[SecuritySafeCritical]
		private void LogData( long num, Dictionary<string, List<ProfilerSample>> data ) {
			try {
				var name = AppDomain.CurrentDomain.FriendlyName;
				var group = num;
				var lines = new List<string>();
				foreach( var kv in data ) {
					var id = kv.Key;
					var polls = kv.Value;
					var stats = string.Format( "{0}, {1:0.00}, {2:0.00}, {3:0.00}, {4:0.00}, {5:0.00}, {6:0.00}, {7:0.00}, {8:0.00}",
						polls.Count(),
						polls.Min( p => p.ElapsedMilliseconds ),
						polls.Average( p => p.ElapsedMilliseconds ),
						polls.Max( p => p.ElapsedMilliseconds ),
						polls.Sum( p => p.ElapsedMilliseconds ),
						polls.Min( p => p.FunctionCalls ),
						polls.Average( p => p.FunctionCalls ),
						polls.Max( p => p.FunctionCalls ),
						polls.Sum( p => p.FunctionCalls ) );
					lines.Add( $"{name}, {num}, {id}, {stats}" );
				}

				//name, group, type, method, count, min, avg, max, sum
				lock( LogLock )
					System.IO.File.AppendAllText( GetLogPath(), string.Join( "\n", lines ) + "\n" );
			}
			catch( Exception ex ) {
				Debug.WriteLine( "ERROR: Failed to append to profiling log" );
				Debug.WriteLine( ex.Message );
			}
		}

		[SecuritySafeCritical]
		private bool InvokeTryExecuteTask( Task task ) {
			var actionObj = TaskActionField.GetValue( task );

			if( LogCounter == 0 ) {
				EnsureParentDirExists( GetLogPath() );
				System.IO.File.WriteAllText( GetLogPath(), "name, group, type, method, count, msMin, msAvg, msMax, msSum, fcMin, fcAvg, fcMax, fcSum\n" );
			}

			if( DateTime.UtcNow - LastLog > LogInterval ) {
				var id = Interlocked.Increment( ref LogCount );
				var prof = TaskProfiling;
				ThreadPool.QueueUserWorkItem( ( o ) => LogData( id, prof ) );
				TaskProfiling = new Dictionary<string, List<ProfilerSample>>();
				LastLog = DateTime.UtcNow;
			}

			var currentCount = ++LogCounter;
			var callCountStart = Function.CallCount;
			var timer = System.Diagnostics.Stopwatch.StartNew();

			var ret = TryExecuteTask( task );
			timer.Stop();
			var callCountEnd = Function.CallCount;

			//Don't log parent tasks. We want only leafs right now
			if( currentCount == LogCounter ) {
				var del = actionObj as MulticastDelegate;
				if( del != null ) {
					var id = $"{del.Method.DeclaringType.ToString()}, {del.Method.ToString()}";
					if( !TaskProfiling.TryGetValue( id, out var list ) )
						TaskProfiling[id] = list = new List<ProfilerSample>();
					list.Add( new ProfilerSample( timer.ElapsedMilliseconds, callCountEnd - callCountStart ) );
				}
			}

			return ret;
		}

		[SecuritySafeCritical]
        public static void Create()
        {
            Instance = new CitizenTaskScheduler();

            Factory = new TaskFactory(Instance);

			TaskScheduler.UnobservedTaskException += TaskScheduler_UnobservedTaskException;

			var field = typeof(TaskScheduler).GetField("s_defaultTaskScheduler", BindingFlags.Static | BindingFlags.NonPublic);
			field.SetValue(null, Instance);

			field = typeof(Task).GetField("s_factory", BindingFlags.Static | BindingFlags.NonPublic);
			field.SetValue(null, Factory);
		}

		private static void TaskScheduler_UnobservedTaskException(object sender, UnobservedTaskExceptionEventArgs e)
		{
			Debug.WriteLine($"Unhandled task exception: {e.Exception.InnerExceptions.Aggregate("", (a, b) => $"{a}\n{b}")}");

			e.SetObserved();
		}

		public static TaskFactory Factory { get; private set; }

        public static CitizenTaskScheduler Instance { get; private set; }
	}

	internal struct ProfilerSample
	{
		public double ElapsedMilliseconds;
		public long FunctionCalls;

		public ProfilerSample( double elapsedMilliseconds, long functionCalls ) {
			ElapsedMilliseconds = elapsedMilliseconds;
			FunctionCalls = functionCalls;
		}
	}
}
