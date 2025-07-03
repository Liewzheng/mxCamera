
# 检查 adb 是否存在
try {
    $adbPath = Get-Command adb -ErrorAction Stop
    Write-Host "ADB found at: $($adbPath.Source)" -ForegroundColor Green
} catch {
    Write-Error "ADB not found! Please install Android SDK Platform Tools and add to PATH."
    Write-Host "Download from: https://developer.android.com/studio/releases/platform-tools" -ForegroundColor Yellow
    exit 1
}

# 继续执行 adb 相关操作
Write-Host "ADB is available, proceeding with deployment..."

# 将 mxCamera 项目编译后的二进制文件推送到设备
adb push build/bin/mxCamera /root/Workspace/mxCamera
Write-Host "mxCamera binary pushed to /root/Workspace/mxCamera on the device." -ForegroundColor Green

# 将 mxcamera 文件推送到设备的 /etc/init.d/S99mxcamera
adb push mxcamera /etc/init.d/S99mxcamera
Write-Host "mxcamera script pushed to /etc/init.d/S99mxcamera on the device." -ForegroundColor Green

# 编写一个数组，通过 adb 禁用 /etc/init.d/ 中的某些启动服务
$servicesToDisable = @(
    "S50telnet",
    "S60micinit",
    "S99hciinit",
    "S99python"
)

foreach ($service in $servicesToDisable) {
    $servicePath = "/etc/init.d/$service"
    if (adb shell "[ -f $servicePath ]") {
        adb shell "mv $servicePath ${servicePath}.disabled"
        Write-Host "Disabled service: $service" -ForegroundColor Green
    } else {
        Write-Host "Service $service does not exist, skipping." -ForegroundColor Yellow
    }
}