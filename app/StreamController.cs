using System;
using System.Threading.Tasks;
using System.Windows.Threading;

namespace CRTCastWin;

/// App-side state around the native engine: the Windows counterpart of CRTCastMac/MirrorController.swift.
/// The AirPlay receiver runs all the time. Streaming to the MiSTer starts on its own when the phone mirrors
/// and stops when it disconnects; brief AirPlay hiccups (under 4 s) don't restart the stream, because a
/// restart re-sends the video mode, which makes the CRT re-lock.
internal sealed class StreamController
{
    public const string ReceiverName = "CRTCast Windows";

    public Settings Settings { get; } = Settings.Load();
    public Native.Stats Stats { get; private set; }
    public Native.AirPlayStatus AirPlay { get; private set; }
    public string AirPlayLine { get; private set; } = "AirPlay: starting…";
    public bool Streaming { get; private set; }
    /// User pressed Stop while the phone was mirroring: don't restart until it disconnects (or Resume).
    public bool Paused { get; private set; }
    public string? Error { get; private set; }
    public event Action? Changed;

    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(250) };
    private int _source = Native.SourceTestPattern;   // what is (or was last) streaming
    private string? _airplayError;
    private int _ticks;
    private bool _wasMirroring;
    private int _notMirroringFor;                      // seconds
    private DateTime _lastAttempt = DateTime.MinValue;

    public void Start()
    {
        _timer.Tick += (_, _) => Tick();
        _timer.Start();
        // Advertising takes a moment, so keep it off the UI thread.
        Task.Run(() =>
        {
            int rc = Native.AirPlayStart(ReceiverName);
            if (rc != 0)
            {
                Native.AirPlayGetStatus(out var s);
                _airplayError = $"AirPlay receiver didn't start ({rc}): {s.LastErrorText}";
            }
        });
    }

    public bool PhoneConnected => AirPlay.Connections > 0;
    public bool PhoneMirroring => AirPlay.Mirroring != 0 && AirPlay.Connections > 0;
    public bool StreamingMirror => Streaming && _source == Native.SourceAirPlay;

    public string Headline =>
        Streaming ? (_source == Native.SourceTestPattern ? "Test pattern on the MiSTer" : "Mirroring to the MiSTer")
        : Paused ? "Stopped (disconnect the phone or press Resume)"
        : PhoneMirroring ? "iPhone is mirroring"
        : PhoneConnected ? "Phone connected"
        : "Waiting for your iPhone";

    public void StartTestPattern() => StartSource(Native.SourceTestPattern);

    private void StartSource(int source)
    {
        Error = null;
        var config = Settings.ToConfig();
        if (config == null)
        {
            Error = "Set the MiSTer's IP address (like 192.168.0.13).";
            Changed?.Invoke();
            return;
        }
        Settings.Save();
        var c = config.Value;
        int rc = Native.Start(ref c, source);
        if (rc != 0) Error = rc == -1 ? "Already streaming." : "Couldn't start (settings).";
        else _source = source;
        Paused = false;
        Refresh();
    }

    public void StopStreaming()
    {
        // Stopping the mirror pauses it (it would restart by itself otherwise); stopping the test pattern doesn't.
        if (StreamingMirror) Paused = true;
        Native.Stop();
        Refresh();
    }

    public void Resume()
    {
        Paused = false;
        _lastAttempt = DateTime.MinValue;
        if (!PhoneMirroring) StartSource(_source);   // with the phone mirroring, Tick starts the mirror
        Refresh();
    }

    /// Fade out and close the stream, and stop the receiver (on quit).
    public void Shutdown()
    {
        _timer.Stop();
        Native.Stop();
        Native.WaitStopped(3000);
        Native.AirPlayStop();
        Settings.Save();
    }

    public string AirPlayLog() => Native.AirPlayLog();

    private void Tick()
    {
        _ticks++;
        if (_ticks % 2 == 0) UpdateAirPlay();
        if (_ticks % 4 == 0) AutoStartStop();   // once a second
        Refresh();
    }

    private void Refresh()
    {
        Native.GetStats(out var s);
        Stats = s;
        Streaming = s.Running != 0;
        Changed?.Invoke();
    }

    private void AutoStartStop()
    {
        bool mirroring = PhoneMirroring;
        _notMirroringFor = mirroring ? 0 : _notMirroringFor + 1;
        if (mirroring || (_wasMirroring && _notMirroringFor < 4))
        {
            if (!mirroring) return;   // brief hiccup: keep everything as it is
            _wasMirroring = true;
            if (!Streaming && !Paused && (DateTime.Now - _lastAttempt).TotalSeconds > 5)
            {
                _lastAttempt = DateTime.Now;
                StartSource(Native.SourceAirPlay);
            }
        }
        else
        {
            if (_wasMirroring)
            {
                _wasMirroring = false;
                Native.AirPlayResetMedia();
                if (StreamingMirror) Native.Stop();
            }
            Paused = false;
        }
    }

    private void UpdateAirPlay()
    {
        Native.AirPlayGetStatus(out var a);
        AirPlay = a;
        if (_airplayError != null) { AirPlayLine = _airplayError; return; }
        if (a.Running == 0) { AirPlayLine = "AirPlay: starting…"; return; }

        var line = $"AirPlay: \"{ReceiverName}\" on port {a.Port}";
        if (a.Connections > 0)
        {
            line += $" · {a.ClientNameText} ({a.ClientModelText})";
            if (a.Mirroring != 0)
                line += $" · {Native.AirPlayVideoSummary()} · audio {AudioCodecName(a.AudioCodec)} packets {a.AudioPackets}";
        }
        else if (!string.IsNullOrEmpty(a.LastEventText)) line += $" · {a.LastEventText}";
        AirPlayLine = line;
    }

    private static string AudioCodecName(int ct) => ct switch { 2 => "ALAC", 4 => "AAC", 8 => "AAC-ELD", _ => "–" };
}
