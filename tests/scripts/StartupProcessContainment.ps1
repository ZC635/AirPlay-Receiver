# Test-only Windows launch primitives. The primary thread stays suspended until
# assignment to our private job; descendants can only be created afterward.
Add-Type @"
using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
public static class AirPlayStartupTestJob {
    [StructLayout(LayoutKind.Sequential)] struct Basic {
        public long ProcessTime, JobTime;
        public uint Flags;
        public UIntPtr MinimumWorkingSet, MaximumWorkingSet;
        public uint ActiveProcesses;
        public UIntPtr Affinity;
        public uint Priority, Scheduling;
    }
    [StructLayout(LayoutKind.Sequential)] struct IoCounters {
        public ulong ReadOperations, WriteOperations, OtherOperations;
        public ulong ReadBytes, WriteBytes, OtherBytes;
    }
    [StructLayout(LayoutKind.Sequential)] struct Extended {
        public Basic BasicLimits;
        public IoCounters Io;
        public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
    }
    [StructLayout(LayoutKind.Sequential)] struct SecurityAttributes {
        public uint Length;
        public IntPtr Descriptor;
        [MarshalAs(UnmanagedType.Bool)] public bool Inherit;
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct StartupInfo {
        public uint Size;
        public string Reserved, Desktop, Title;
        public uint X, Y, Width, Height, XChars, YChars, Fill, Flags;
        public ushort ShowWindow, ReservedBytes;
        public IntPtr ReservedPointer, Input, Output, Error;
    }
    [StructLayout(LayoutKind.Sequential)] struct ProcessInformation {
        public IntPtr Process, Thread;
        public uint ProcessId, ThreadId;
    }
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetInformationJobObject(IntPtr job, int type, ref Extended info, uint size);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode, ExactSpelling=true)]
    static extern bool CreateProcessW(string application, StringBuilder command, IntPtr processAttributes, IntPtr threadAttributes,
        bool inherit, uint flags, IntPtr environment, string directory, ref StartupInfo startup, out ProcessInformation process);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode, ExactSpelling=true)]
    static extern IntPtr CreateFileW(string path, uint access, uint share, ref SecurityAttributes attributes, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool TerminateProcess(IntPtr process, uint code);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint WaitForSingleObject(IntPtr handle, uint timeout);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetExitCodeProcess(IntPtr process, out uint code);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
    public static IntPtr Create() {
        IntPtr job = CreateJobObject(IntPtr.Zero, null);
        if (job == IntPtr.Zero) throw new Win32Exception();
        Extended info = new Extended();
        info.BasicLimits.Flags = 0x2000; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if (!SetInformationJobObject(job, 9, ref info, (uint)Marshal.SizeOf(info))) {
            int error = Marshal.GetLastWin32Error(); CloseHandle(job);
            throw new Win32Exception(error);
        }
        return job;
    }
    public static void Assign(IntPtr job, IntPtr process) {
        if (!AssignProcessToJobObject(job, process)) throw new Win32Exception();
    }
    static IntPtr OpenInheritedFile(string path, uint access, uint disposition) {
        SecurityAttributes attributes = new SecurityAttributes();
        attributes.Length = (uint)Marshal.SizeOf(attributes);
        attributes.Inherit = true;
        IntPtr handle = CreateFileW(path, access, 3, ref attributes, disposition, 0x80, IntPtr.Zero);
        if (handle == new IntPtr(-1)) throw new Win32Exception();
        return handle;
    }
    public sealed class SuspendedChild : IDisposable {
        IntPtr process, thread;
        public int Id { get; private set; }
        internal SuspendedChild(IntPtr process, IntPtr thread, int id) {
            this.process = process; this.thread = thread; Id = id;
        }
        public bool WaitForExit(int timeout) {
            uint result = WaitForSingleObject(process, (uint)timeout);
            if (result == 0) return true;
            if (result == 258) return false;
            throw new Win32Exception();
        }
        public int ExitCode {
            get {
                if (!WaitForExit(0)) throw new InvalidOperationException("Child is still running");
                uint code;
                if (!GetExitCodeProcess(process, out code)) throw new Win32Exception();
                return unchecked((int)code);
            }
        }
        public void EnrollAndResume(IntPtr job) {
            try {
                Assign(job, process);
                if (ResumeThread(thread) != 1) throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not resume the suspended child");
            } catch {
                // Assignment failure still owns a suspended process. Terminate it
                // before releasing handles; it must never execute uncontained.
                TerminateAndWait();
                throw;
            }
        }
        void TerminateAndWait() {
            if (WaitForSingleObject(process, 0) == 0) return;
            if (!TerminateProcess(process, 1)) throw new Win32Exception();
            if (WaitForSingleObject(process, 5000) != 0) throw new InvalidOperationException("Owned child termination timed out");
        }
        public void Dispose() {
            if (process == IntPtr.Zero) return;
            try { TerminateAndWait(); }
            finally { CloseHandle(thread); CloseHandle(process); thread = process = IntPtr.Zero; }
        }
    }
    public static SuspendedChild StartSuspended(ProcessStartInfo start, string stdout, string stderr) {
        IntPtr output = IntPtr.Zero, error = IntPtr.Zero, input = IntPtr.Zero, environment = IntPtr.Zero;
        try {
            // File redirection avoids pipe-EOF waits even during forced cleanup.
            output = OpenInheritedFile(stdout, 0x40000000, 2); // write, CREATE_ALWAYS
            error = OpenInheritedFile(stderr, 0x40000000, 2);
            input = OpenInheritedFile("NUL", 0x80000000, 3); // read, OPEN_EXISTING
            List<string> entries = new List<string>();
            foreach (DictionaryEntry entry in start.EnvironmentVariables) entries.Add(entry.Key + "=" + entry.Value);
            entries.Sort(StringComparer.OrdinalIgnoreCase);
            environment = Marshal.StringToHGlobalUni(String.Join("\0", entries.ToArray()) + "\0\0");
            StartupInfo startup = new StartupInfo();
            startup.Size = (uint)Marshal.SizeOf(startup);
            startup.Flags = 0x101; // STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW
            startup.Input = input; startup.Output = output; startup.Error = error;
            ProcessInformation created;
            StringBuilder command = new StringBuilder("\"" + start.FileName + "\" " + start.Arguments);
            const uint flags = 0x08000000 | 0x00000400 | 0x00000004; // NO_WINDOW | UNICODE_ENVIRONMENT | SUSPENDED
            if (!CreateProcessW(start.FileName, command, IntPtr.Zero, IntPtr.Zero, true, flags,
                                environment, start.WorkingDirectory, ref startup, out created)) throw new Win32Exception();
            return new SuspendedChild(created.Process, created.Thread, (int)created.ProcessId);
        } finally {
            if (environment != IntPtr.Zero) Marshal.FreeHGlobal(environment);
            if (input != IntPtr.Zero) CloseHandle(input);
            if (error != IntPtr.Zero) CloseHandle(error);
            if (output != IntPtr.Zero) CloseHandle(output);
        }
    }
}
"@
