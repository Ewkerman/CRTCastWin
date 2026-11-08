using System;
using System.Drawing;
using System.Threading;
using System.Windows;
using Forms = System.Windows.Forms;

namespace CRTCastWin;

/// CRTCast for Windows. Lives in the system tray (Stop/Resume there), so the window can stay closed.
public partial class App : Application
{
    private Mutex? _single;
    private Forms.NotifyIcon? _tray;
    private Forms.ToolStripMenuItem? _statusItem, _testItem, _stopItem, _resumeItem;
    private MainWindow? _window;
    internal StreamController Controller { get; } = new();

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        _single = new Mutex(true, "CRTCast.Windows.SingleInstance", out bool first);
        if (!first)
        {
            MessageBox.Show("CRTCast is already running (see the tray icon).", "CRTCast");
            Shutdown();
            return;
        }

        try { _ = Native.Version; }
        catch (DllNotFoundException)
        {
            MessageBox.Show("crtcast_core.dll is missing next to CRTCastWin.exe. Build it with CRTCastWin\\build.ps1.", "CRTCast");
            Shutdown();
            return;
        }

        Controller.Start();
        Controller.Changed += UpdateTray;
        CreateTray();
        ShowWindow();
    }

    private void CreateTray()
    {
        var menu = new Forms.ContextMenuStrip();
        _statusItem = new Forms.ToolStripMenuItem("Idle") { Enabled = false };
        _testItem = new Forms.ToolStripMenuItem("Test pattern", null, (_, _) => Controller.StartTestPattern());
        _stopItem = new Forms.ToolStripMenuItem("Stop", null, (_, _) => Controller.StopStreaming());
        _resumeItem = new Forms.ToolStripMenuItem("Resume", null, (_, _) => Controller.Resume());
        menu.Items.Add(_statusItem);
        menu.Items.Add(new Forms.ToolStripSeparator());
        menu.Items.Add(_testItem);
        menu.Items.Add(_stopItem);
        menu.Items.Add(_resumeItem);
        menu.Items.Add(new Forms.ToolStripSeparator());
        menu.Items.Add("Show window", null, (_, _) => ShowWindow());
        menu.Items.Add("Quit CRTCast", null, (_, _) => Quit());

        _tray = new Forms.NotifyIcon
        {
            Icon = LoadTrayIcon(),
            Text = "CRTCast",
            ContextMenuStrip = menu,
            Visible = true,
        };
        _tray.MouseClick += (_, a) => { if (a.Button == Forms.MouseButtons.Left) ShowWindow(); };
        UpdateTray();
    }

    private static Icon LoadTrayIcon()
    {
        try
        {
            var info = GetResourceStream(new Uri("pack://application:,,,/Assets/CRTCast-logo.png"));
            if (info != null)
            {
                using var src = new Bitmap(info.Stream);
                using var small = new Bitmap(src, new System.Drawing.Size(32, 32));
                return Icon.FromHandle(small.GetHicon());
            }
        }
        catch { /* fall back below */ }
        return SystemIcons.Application;
    }

    private void UpdateTray()
    {
        if (_tray == null) return;
        var c = Controller;
        _statusItem!.Text = c.Headline;
        _testItem!.Visible = !c.Streaming;
        _stopItem!.Visible = c.Streaming;
        _resumeItem!.Visible = !c.Streaming && c.Paused;
        var tip = "CRTCast · " + c.Headline;
        _tray.Text = tip.Length > 63 ? tip[..63] : tip;
    }

    internal void ShowWindow()
    {
        if (_window == null)
        {
            _window = new MainWindow(Controller);
            _window.Closed += (_, _) => _window = null;
        }
        _window.Show();
        if (_window.WindowState == WindowState.Minimized) _window.WindowState = WindowState.Normal;
        _window.Activate();
    }

    /// Called when the window is closed: keep running in the tray.
    internal void WindowClosedToTray()
    {
        var s = Controller.Settings;
        if (!s.TrayHintShown && _tray != null)
        {
            s.TrayHintShown = true;
            s.Save();
            _tray.ShowBalloonTip(4000, "CRTCast is still running",
                "It keeps streaming from the tray. Right-click the icon for Stop, Resume and Quit.", Forms.ToolTipIcon.Info);
        }
    }

    internal void Quit()
    {
        Controller.Shutdown();
        if (_tray != null) { _tray.Visible = false; _tray.Dispose(); }
        _single?.ReleaseMutex();
        Shutdown();
    }
}
