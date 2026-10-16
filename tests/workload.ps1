# Desktop activity generator for GPU measurements.
#   -Mode small : a 520x60 text field updated every frame (typing, caret, clock)
#   -Mode large : a 1800x1000 area of scrolling text (browser/editor scrolling)
param([ValidateSet('small', 'large')] [string]$Mode = 'small', [int]$Seconds = 15)

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$w, $h = if ($Mode -eq 'small') { 520, 60 } else { 1800, 1000 }
$form = New-Object System.Windows.Forms.Form
$form.GetType().GetProperty('DoubleBuffered', [Reflection.BindingFlags]'NonPublic,Instance').SetValue($form, $true)
$form.Text = "Fringeless workload ($Mode)"
$form.StartPosition = 'Manual'
$form.Location = New-Object System.Drawing.Point(($screen.Left + ($screen.Width - $w) / 2), ($screen.Top + ($screen.Height - $h) / 2))
$form.ClientSize = New-Object System.Drawing.Size($w, $h)
$form.BackColor = [System.Drawing.Color]::White

$font = New-Object System.Drawing.Font('Segoe UI', 11)
$script:frame = 0
$text = 'Sphinx of black quartz, judge my vow. The quick brown fox jumps over the lazy dog 0123456789'
$form.Add_Paint({
    param($s, $e)
    $g = $e.Graphics
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
    if ($Mode -eq 'small') {
        $g.DrawString(("{0}  {1:D6}" -f $text.Substring(0, 30), $script:frame), $font, [System.Drawing.Brushes]::Black, 8, 18)
    } else {
        $offset = ($script:frame * 3) % 22
        for ($y = -$offset; $y -lt $h; $y += 22) {
            $line = [int](($y + $script:frame * 3) / 22)
            $g.DrawString("$line  $text", $font, [System.Drawing.Brushes]::Black, 8, $y)
        }
    }
})
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1
$sw = [Diagnostics.Stopwatch]::StartNew()
$timer.Add_Tick({
    $script:frame++
    $form.Invalidate()
    if ($sw.Elapsed.TotalSeconds -ge $Seconds) { $form.Close() }
})
$timer.Start()
$form.TopMost = $true
# Started with a hidden console, the first ShowWindow call inherits SW_HIDE; show twice.
$form.Show(); $form.Hide(); $form.Show()
[System.Windows.Forms.Application]::Run($form)
