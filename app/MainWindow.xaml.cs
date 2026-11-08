using System;
using System.ComponentModel;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace CRTCastWin;

public partial class MainWindow : Window
{
    private readonly StreamController _c;
    private readonly Native.ModeInfo[] _modes = new Native.ModeInfo[Native.ModeCount];
    private bool _loading = true;

    private WriteableBitmap? _preview;
    private byte[] _previewBytes = new byte[640 * 480 * 4];
    private ulong _previewSeq;
    private ulong _airplaySeq;

    internal MainWindow(StreamController controller)
    {
        _c = controller;
        InitializeComponent();

        for (int i = 0; i < Native.ModeCount; i++)
        {
            Native.GetModeInfo(i, out _modes[i]);
            ModeBox.Items.Add(_modes[i].NameText);
        }
        for (int ms = -150; ms <= 150; ms += 10)
            DelayBox.Items.Add(new ComboBoxItem { Content = ms == 0 ? "0 ms" : $"{ms:+0;-0} ms", Tag = ms });

        var s = _c.Settings;
        HostBox.Text = s.Host;
        ModeBox.SelectedIndex = Math.Clamp(s.Mode, 0, Native.ModeCount - 1);
        Rgb565Box.IsChecked = s.Rgb565;
        AudioBox.IsChecked = s.SendAudio;
        DelayBox.SelectedIndex = Math.Clamp((s.AudioDelayMs + 150) / 10, 0, DelayBox.Items.Count - 1);
        LowLatencyBox.IsChecked = s.LowLatency;
        VersionText.Text = $"CRTCast for Windows 0.4 (in development) · {Native.Version}";
        _loading = false;
        UpdateSettingsUi();

        _c.Changed += Refresh;
        Refresh();
    }

    // MARK: - Settings

    private void Host_TextChanged(object sender, TextChangedEventArgs e) => Setting_Changed(sender, e);

    private void Setting_Changed(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        var s = _c.Settings;
        s.Host = HostBox.Text.Trim();
        s.Mode = Math.Max(0, ModeBox.SelectedIndex);
        s.Rgb565 = Rgb565Box.IsChecked == true;
        s.SendAudio = AudioBox.IsChecked == true;
        s.AudioDelayMs = DelayBox.SelectedItem is ComboBoxItem { Tag: int ms } ? ms : 0;
        s.LowLatency = LowLatencyBox.IsChecked == true;
        if (sender != HostBox || e.RoutedEvent == LostFocusEvent) s.Save();
        UpdateSettingsUi();
    }

    private void UpdateSettingsUi()
    {
        var s = _c.Settings;
        var m = _modes[Math.Clamp(s.Mode, 0, Native.ModeCount - 1)];
        double mbps = s.Rgb565 ? m.MbpsRgb565 : m.MbpsRgb888;
        BandwidthText.Text = $"Needed: {mbps:0} Mbps. Wired Ethernet is best. On Wi-Fi, if the picture " +
                             "stutters, try RGB565 or 320×240p / 640×240p.";
    }

    // MARK: - Buttons

    private void Test_Click(object sender, RoutedEventArgs e) { Setting_Changed(HostBox, new RoutedEventArgs(LostFocusEvent)); _c.StartTestPattern(); }
    private void Stop_Click(object sender, RoutedEventArgs e) => _c.StopStreaming();
    private void Resume_Click(object sender, RoutedEventArgs e) { Setting_Changed(HostBox, new RoutedEventArgs(LostFocusEvent)); _c.Resume(); }

    // MARK: - Status + preview

    private void Refresh()
    {
        var st = _c.Stats;
        bool streaming = _c.Streaming;

        Headline.Text = _c.Headline;
        StatusDot.Fill = streaming ? (Brush)FindResource("Phosphor") : _c.PhoneConnected ? Brushes.Gold : Brushes.Gray;
        AirPlayText.Text = _c.AirPlayLine;
        StatusGlow.Opacity = streaming ? 0.9 : 0;
        TestButton.Visibility = streaming ? Visibility.Collapsed : Visibility.Visible;
        StopButton.Visibility = streaming ? Visibility.Visible : Visibility.Collapsed;
        ResumeButton.Visibility = !streaming && _c.Paused ? Visibility.Visible : Visibility.Collapsed;

        string? error = _c.Error ?? (string.IsNullOrEmpty(st.ErrorText) ? null : st.ErrorText);
        ErrorText.Text = error ?? "";
        ErrorText.Visibility = error == null ? Visibility.Collapsed : Visibility.Visible;

        if (streaming || st.Blits > 0)
        {
            var ci = CultureInfo.InvariantCulture;
            StatusLines.Text = string.Format(ci,
                "{0}\nMiSTer: {1} · {2:0.0} fields/s · late fields {3} · catch-ups {4} · sound {5}\n" +
                "pack {6:0.00} ms (max {10:0.0}) · send {7:0.00} ms (max {11:0.0}) · audio {8:0.0} s\n{9}",
                st.StateText,
                st.MisterSynced != 0 ? "in sync" : "not synced", st.BlitsPerSecond, st.LateFields, st.AudioCatchUps,
                st.MisterAudio != 0 ? "on" : "OFF in MiSTer OSD",
                st.PackMs, st.SendMs, st.AudioSeconds, st.SourceStatusText, st.PackMaxMs, st.SendMaxMs)
                + (st.NetQueueOverflowed != 0 ? "\nWarning: network send queue overflowed" : "");
        }
        else
        {
            StatusLines.Text = "";
        }

        UpdatePreview(streaming);
    }

    private void UpdatePreview(bool streaming)
    {
        // The phone's screen while it mirrors (even before/without the MiSTer stream); otherwise what the MiSTer gets.
        bool phone = _c.PhoneMirroring && !(streaming && !_c.StreamingMirror);
        if (!streaming && !phone)
        {
            PreviewHint.Visibility = Visibility.Visible;
            PreviewImage.Source = null;
            _airplaySeq = 0;
            return;
        }
        int w, h;
        if (phone)
        {
            if (Native.AirPlayGetPreview(_previewBytes, _previewBytes.Length, ref _airplaySeq) == 0) return;
            w = 640;
            h = 480;
        }
        else if (Native.GetPreview(_previewBytes, _previewBytes.Length, out w, out h, ref _previewSeq) == 0) return;
        if (_preview == null || _preview.PixelWidth != w || _preview.PixelHeight != h)
            _preview = new WriteableBitmap(w, h, 96, 96, PixelFormats.Bgr32, null);
        _preview.WritePixels(new Int32Rect(0, 0, w, h), _previewBytes, w * 4, 0);
        PreviewImage.Source = _preview;
        PreviewHint.Visibility = Visibility.Collapsed;
    }

    private void ShowLog_Click(object sender, RoutedEventArgs e)
    {
        var box = new TextBox
        {
            Text = _c.AirPlayLog(), IsReadOnly = true, FontFamily = new FontFamily("Consolas"), FontSize = 11.5,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            TextWrapping = TextWrapping.NoWrap,
        };
        var w = new Window { Title = "CRTCast · AirPlay log", Width = 820, Height = 480, Owner = this, Content = box,
                             Background = Brushes.Black };
        box.ScrollToEnd();
        w.Show();
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        _c.Changed -= Refresh;
        base.OnClosing(e);
        ((App)Application.Current).WindowClosedToTray();
    }
}
