# Capture the console's klog (port 3232) into logs/klog.txt, reconnecting if it drops.
param([Parameter(Mandatory)][string]$Ip, [int]$Minutes = 180)

$out = Join-Path $PSScriptRoot "..\logs\klog.txt"
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
$end = (Get-Date).AddMinutes($Minutes)
$buf = New-Object byte[] 16384

while ((Get-Date) -lt $end) {
    try {
        $c = New-Object Net.Sockets.TcpClient($Ip, 3232)
        $s = $c.GetStream()
        $w = [IO.File]::Open($out, 'Append', 'Write', 'ReadWrite')
        $last = Get-Date
        while ($c.Connected -and (Get-Date) -lt $end) {
            if ($s.DataAvailable) {
                $n = $s.Read($buf, 0, $buf.Length)
                if ($n -le 0) { break }
                $w.Write($buf, 0, $n)
                $w.Flush()
                $last = Get-Date
            } elseif (((Get-Date) - $last).TotalSeconds -gt 20) {
                # rest mode leaves the socket half dead, Connected stays true
                break
            } else {
                Start-Sleep -Milliseconds 100
            }
        }
        $w.Close()
        $c.Close()
    } catch {
        Start-Sleep -Seconds 5
    }
}
