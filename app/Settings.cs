using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text.Json;

namespace CRTCastWin;

/// User settings, saved to %APPDATA%\CRTCast\settings.json.
/// LZ4, jumbo frames and field swapping are always off on Windows: LZ4 makes 480i jitter up and down
/// here (not on the Mac, cause unknown), and the other two aren't needed.
public sealed class Settings
{
    public string Host { get; set; } = "";
    public int Mode { get; set; } = 2;               // 640×480i
    /// RGB565 needs a third less bandwidth but shows banding; RGB888 looks best and fits wired Ethernet easily.
    public bool Rgb565 { get; set; }
    public bool SendAudio { get; set; } = true;
    public int AudioDelayMs { get; set; }
    public bool LowLatency { get; set; }
    public bool TrayHintShown { get; set; }

    private static string FilePath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "CRTCast", "settings.json");

    public static Settings Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<Settings>(File.ReadAllText(FilePath)) ?? new Settings();
        }
        catch { /* unreadable settings: start fresh */ }
        return new Settings();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch { /* not fatal */ }
    }

    /// Stream settings for the native engine, or null if the IP isn't a valid IPv4 address.
    internal Native.Config? ToConfig()
    {
        var host = Host.Trim();
        if (!IPAddress.TryParse(host, out var ip) || ip.AddressFamily != AddressFamily.InterNetwork) return null;
        return new Native.Config
        {
            Host = Native.Utf8Fixed(host, 64),
            Mode = Math.Clamp(Mode, 0, Native.ModeCount - 1),
            Rgb565 = Rgb565 ? 1 : 0,
            Lz4 = 0,
            Jumbo = 0,
            SwapFields = 0,
            SendAudio = SendAudio ? 1 : 0,
            LowLatency = LowLatency ? 1 : 0,
            AudioDelayMs = AudioDelayMs,
        };
    }
}
