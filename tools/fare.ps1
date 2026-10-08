# STM32'den seri porttan gelen fare komutlarını gerçek imlece uygular.
#
# STM32'nin gönderdiği satırlar:
#   M dx dy   -> imleci dx, dy piksel kaydır   (örnek: "M 4 -4")
#   B L 1/0   -> sol tık bas / bırak
#   B R 1/0   -> sağ tık bas / bırak
#
# Kullanım: .\fare.ps1            (varsayılan COM7)
#           .\fare.ps1 -Port COM5
# Çıkış: Ctrl+C.  Not: Aynı anda 'izle' açıksa COM7 meşgul olur, önce onu kapat.
param([string]$Port = 'COM7')

# Windows'un fare fonksiyonunu (user32.dll) PowerShell'den çağırabilmek için
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Fare {
    [DllImport("user32.dll")]
    public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
}
"@

$HAREKET   = 0x0001
$SOL_BAS   = 0x0002
$SOL_BIRAK = 0x0004
$SAG_BAS   = 0x0008
$SAG_BIRAK = 0x0010

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$sp.ReadTimeout = 30          # kısa tut ki klavyeye de sık bakabilelim
$sp.Open()
Write-Host "$Port dinleniyor, STM32 fare aktif. (Ctrl+C ile çık)"
Write-Host "Menüye sayı yazıp Enter'a basarak ayar yapabilirsin."

$klavye_var = $true
$girdi = ''

try {
    while ($true) {
        # ---- Klavye: yazılan sayıları topla, Enter'da karta gönder ----
        if ($klavye_var) {
            try {
                while ([Console]::KeyAvailable) {
                    $k = [Console]::ReadKey($true)
                    if ($k.Key -eq 'Enter') {
                        Write-Host ''
                        $sp.Write($girdi + "`r`n")
                        $girdi = ''
                    }
                    elseif ($k.Key -eq 'Backspace') {
                        if ($girdi.Length -gt 0) {
                            $girdi = $girdi.Substring(0, $girdi.Length - 1)
                            Write-Host -NoNewline "`b `b"
                        }
                    }
                    elseif ($k.KeyChar -match '[0-9]') {
                        $girdi += $k.KeyChar
                        Write-Host -NoNewline $k.KeyChar
                    }
                }
            }
            catch {
                $klavye_var = $false
                Write-Host "Bu terminalde klavye okunamıyor. Ayar yapmak için C:\Users\yavuz\fare.bat dosyasına çift tıklayıp ayrı pencerede aç."
            }
        }

        # ---- Seri port: karttan gelen satırları işle ----
        try { $satir = $sp.ReadLine().Trim() } catch [System.TimeoutException] { continue }
        $p = $satir -split ' '

        if ($p[0] -eq 'M' -and $p.Count -eq 3) {
            [Fare]::mouse_event($HAREKET, [int]$p[1], [int]$p[2], 0, [UIntPtr]::Zero)
        }
        elseif ($p[0] -eq 'B' -and $p.Count -eq 3) {
            if ($p[1] -eq 'L') { $f = if ($p[2] -eq '1') { $SOL_BAS } else { $SOL_BIRAK } }
            else               { $f = if ($p[2] -eq '1') { $SAG_BAS } else { $SAG_BIRAK } }
            [Fare]::mouse_event($f, 0, 0, 0, [UIntPtr]::Zero)
        }
        elseif ($satir) {
            Write-Host $satir   # tanımadığımız satırları (açılış mesajı vb.) ekrana yaz
        }
    }
}
finally {
    $sp.Close()
}
