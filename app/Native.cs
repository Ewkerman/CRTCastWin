using System;
using System.Runtime.InteropServices;
using System.Text;

namespace CRTCastWin;

/// P/Invoke over crtcast_core.dll (CRTCastWin/core/include/crtcast_core.h).
/// Layouts must match the C structs field for field.
internal static class Native
{
    private const string Dll = "crtcast_core.dll";

    public const int ModeCount = 3;
    public const int SourceTestPattern = 0;
    public const int SourceAirPlay = 1;

    [StructLayout(LayoutKind.Sequential)]
    public struct Config
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public byte[] Host;
        public int Mode;
        public int Rgb565;
        public int Lz4;
        public int Jumbo;
        public int SwapFields;
        public int SendAudio;
        public int LowLatency;
        public double AudioDelayMs;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Stats
    {
        public int Running;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 96)] public byte[] State;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public byte[] Error;
        public ulong Blits;
        public double BlitsPerSecond;
        public ulong NewPictures;
        public double PackMs;
        public double SendMs;
        public int MisterSynced;
        public int MisterAudio;
        public uint MisterFrame;
        public double AudioSeconds;
        public int AudioCatchUps;
        public ulong LateFields;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public byte[] SourceStatus;
        public ulong NetSendsCompleted;
        public int NetQueueOverflowed;
        public double PackMaxMs;
        public double SendMaxMs;

        public string StateText => Text(State);
        public string ErrorText => Text(Error);
        public string SourceStatusText => Text(SourceStatus);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ModeInfo
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public byte[] Name;
        public int Width;
        public int LinesPerBlit;
        public int Interlaced;
        public double RefreshHz;
        public double MbpsRgb888;
        public double MbpsRgb565;

        public string NameText => Text(Name);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct AirPlayStatus
    {
        public int Running;
        public int Connections;
        public int TotalConnections;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 128)] public byte[] ClientName;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public byte[] ClientModel;
        public int Mirroring;
        public int Codec;
        public int Width, Height;
        public ulong VideoFrames;
        public ulong VideoBytes;
        public ulong AudioPackets;
        public int AudioCodec;
        public ushort Port;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 160)] public byte[] LastEvent;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 160)] public byte[] LastError;

        public string ClientNameText => Text(ClientName);
        public string ClientModelText => Text(ClientModel);
        public string LastEventText => Text(LastEvent);
        public string LastErrorText => Text(LastError);
    }

    [DllImport(Dll, EntryPoint = "cc_airplay_start")]
    private static extern int cc_airplay_start(byte[] nameUtf8);
    public static int AirPlayStart(string name) => cc_airplay_start(Utf8Fixed(name, 128));
    [DllImport(Dll, EntryPoint = "cc_airplay_stop")] public static extern void AirPlayStop();
    [DllImport(Dll, EntryPoint = "cc_airplay_get_status")] public static extern void AirPlayGetStatus(out AirPlayStatus status);
    [DllImport(Dll, EntryPoint = "cc_airplay_reset_media")] public static extern void AirPlayResetMedia();
    [DllImport(Dll, EntryPoint = "cc_airplay_get_preview")]
    public static extern int AirPlayGetPreview(byte[] bgra, int capacity, ref ulong seq);
    [DllImport(Dll, EntryPoint = "cc_airplay_get_video_summary")] private static extern void cc_airplay_get_video_summary(byte[] buffer, int size);
    public static string AirPlayVideoSummary()
    {
        var b = new byte[256];
        cc_airplay_get_video_summary(b, b.Length);
        return Text(b);
    }
    [DllImport(Dll, EntryPoint = "cc_airplay_get_log")] private static extern void cc_airplay_get_log(byte[] buffer, int size);
    public static string AirPlayLog()
    {
        var b = new byte[16384];
        cc_airplay_get_log(b, b.Length);
        return Text(b);
    }

    [DllImport(Dll, EntryPoint = "cc_version")] private static extern IntPtr cc_version();
    [DllImport(Dll, EntryPoint = "cc_get_mode_info")] public static extern int GetModeInfo(int mode, out ModeInfo info);
    [DllImport(Dll, EntryPoint = "cc_start")] public static extern int Start(ref Config config, int source);
    [DllImport(Dll, EntryPoint = "cc_stop")] public static extern void Stop();
    [DllImport(Dll, EntryPoint = "cc_wait_stopped")] public static extern int WaitStopped(int timeoutMs);
    [DllImport(Dll, EntryPoint = "cc_get_stats")] public static extern void GetStats(out Stats stats);
    [DllImport(Dll, EntryPoint = "cc_get_preview")]
    public static extern int GetPreview(byte[] bgra, int capacity, out int width, out int height, ref ulong seq);

    public static string Version => Marshal.PtrToStringUTF8(cc_version()) ?? "";

    public static byte[] Utf8Fixed(string s, int size)
    {
        var buf = new byte[size];
        var bytes = Encoding.UTF8.GetBytes(s);
        Array.Copy(bytes, buf, Math.Min(bytes.Length, size - 1));
        return buf;
    }

    public static string Text(byte[]? b)
    {
        if (b == null) return "";
        int n = Array.IndexOf(b, (byte)0);
        return Encoding.UTF8.GetString(b, 0, n < 0 ? b.Length : n);
    }
}
