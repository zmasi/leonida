# Leonida: NVIDIA Smooth Motion (the driver's frame generation, RTX 40 and 50) for leonida_sm.exe.
#   scripts\smooth-motion.ps1            what the profile says now
#   scripts\smooth-motion.ps1 -State on  doubles the frames shown (the game renders the same number)
#   scripts\smooth-motion.ps1 -State off
# It writes one per-application setting in the driver's own profile store through NVAPI (no administrator, no other
# application touched); the NVIDIA App should be closed while it runs. It takes effect at the next start.
# The setting's ID (0xB0D384C0, "Smooth Motion - Enable") was found by lepotane/VIctory-leonida (GPL-3.0); the
# NVAPI entry points are the documented ones of nvapi64.dll's query interface.
param(
    [ValidateSet('query', 'on', 'off')][string]$State = 'query',
    [string]$Exe = 'leonida_sm.exe'
)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class LeonidaDrs {
    const uint SmoothMotion = 0xB0D384C0;

    [StructLayout(LayoutKind.Sequential, Pack = 8, CharSet = CharSet.Unicode)]
    struct Setting {
        public uint version;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 2048)] public string name;
        public uint id; public int type; public int location; public uint isCurrentPredefined; public uint isPredefinedValid;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4100)] public byte[] predefined;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4100)] public byte[] current;
    }
    [StructLayout(LayoutKind.Sequential, Pack = 8, CharSet = CharSet.Unicode)]
    struct Profile {
        public uint version;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 2048)] public string name;
        public uint gpuSupport; public uint isPredefined; public uint numApps; public uint numSettings;
    }
    [StructLayout(LayoutKind.Sequential, Pack = 8, CharSet = CharSet.Unicode)]
    struct App {
        public uint version; public uint isPredefined;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 2048)] public string appName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 2048)] public string friendly;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 2048)] public string launcher;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 2048)] public string fileInFolder;
        public uint flags;
    }
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr QueryFn(uint id);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int NoArg();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int OutHandle(ref IntPtr h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int OnHandle(IntPtr h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int FindAppFn(IntPtr h, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder n, ref IntPtr p, ref App a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int NewProfileFn(IntPtr h, ref Profile p, ref IntPtr o);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int NewAppFn(IntPtr h, IntPtr p, ref App a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetFn(IntPtr h, IntPtr p, uint id, ref Setting s, ref uint x);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int SetFn(IntPtr h, IntPtr p, ref Setting s, uint x, uint y);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr LoadLibraryW(string n);
    [DllImport("kernel32.dll")] static extern IntPtr GetProcAddress(IntPtr m, IntPtr ordinal);

    static QueryFn query;
    static T Fn<T>(uint id) where T : class {
        IntPtr p = query(id);
        if (p == IntPtr.Zero) throw new Exception("NVAPI has no entry 0x" + id.ToString("X8"));
        return (T)(object)Marshal.GetDelegateForFunctionPointer(p, typeof(T));
    }
    static uint Ver<T>(int v) { return (uint)(Marshal.SizeOf(typeof(T)) | (v << 16)); }
    static void Check(int status, string what) { if (status != 0) throw new Exception(what + " failed: NVAPI status " + status); }

    // Returns the value before; writes `value` when it is not null. -1: never set for this application.
    public static long Run(string exe, int? value) {
        IntPtr lib = LoadLibraryW("nvapi64.dll");
        if (lib == IntPtr.Zero) throw new Exception("nvapi64.dll not found: is this an NVIDIA driver?");
        query = (QueryFn)Marshal.GetDelegateForFunctionPointer(GetProcAddress(lib, (IntPtr)2), typeof(QueryFn));
        Check(Fn<NoArg>(0x0150E828)(), "NvAPI_Initialize");
        IntPtr session = IntPtr.Zero;
        Check(Fn<OutHandle>(0x0694D52E)(ref session), "DRS_CreateSession");
        try {
            Check(Fn<OnHandle>(0x375DBD6B)(session), "DRS_LoadSettings");
            App app = new App { version = Ver<App>(3), appName = exe, friendly = "Leonida", launcher = "", fileInFolder = "" };
            IntPtr profile = IntPtr.Zero;
            if (Fn<FindAppFn>(0xEEE566B2)(session, new StringBuilder(exe), ref profile, ref app) != 0 || profile == IntPtr.Zero) {
                Profile p = new Profile { version = Ver<Profile>(1), name = "Leonida", gpuSupport = 1 };
                Check(Fn<NewProfileFn>(0xCC176068)(session, ref p, ref profile), "DRS_CreateProfile");
                app = new App { version = Ver<App>(3), appName = exe, friendly = "Leonida", launcher = "", fileInFolder = "" };
                Check(Fn<NewAppFn>(0x4347A9DE)(session, profile, ref app), "DRS_CreateApplication");
            }
            Setting s = new Setting { version = Ver<Setting>(1), name = "", predefined = new byte[4100], current = new byte[4100] };
            uint unused = 0;
            long before = Fn<GetFn>(0xEA99498D)(session, profile, SmoothMotion, ref s, ref unused) == 0 ? (long)BitConverter.ToUInt32(s.current, 0) : -1L;
            if (value.HasValue) {
                Setting w = new Setting { version = Ver<Setting>(1), name = "", id = SmoothMotion, predefined = new byte[4100], current = new byte[4100] };
                Buffer.BlockCopy(BitConverter.GetBytes((uint)value.Value), 0, w.current, 0, 4);
                Check(Fn<SetFn>(0x8A2CF5F5)(session, profile, ref w, 0, 0), "DRS_SetSetting");
                Check(Fn<OnHandle>(0xFCBC7E14)(session), "DRS_SaveSettings");
            }
            return before;
        } finally {
            Fn<OnHandle>(0xDAD9CFF8)(session);
        }
    }
}
'@ -ErrorAction Stop
$ErrorActionPreference = 'Stop'
$value = switch ($State) { 'on' { 1 } 'off' { 0 } default { $null } }
$before = [LeonidaDrs]::Run($Exe, $value)
$was = switch ($before) { -1 { 'not set' } 0 { 'off' } default { 'on' } }
if ($null -eq $value) { "Smooth Motion for ${Exe}: $was" } else { "Smooth Motion for ${Exe}: $was -> $State (from the next start)" }
