
# mxCamera 自动部署脚本
# 通过 ADB 将编译好的程序和库文件部署到 Luckfox Pico 设备

Write-Host "=== mxCamera 部署脚本 ===" -ForegroundColor Cyan
Write-Host "正在检查 ADB 连接状态..." -ForegroundColor Yellow

# 检查 adb 是否存在
try {
    $adbPath = Get-Command adb -ErrorAction Stop
    Write-Host "ADB found at: $($adbPath.Source)" -ForegroundColor Green
} catch {
    Write-Error "ADB not found! Please install Android SDK Platform Tools and add to PATH."
    Write-Host "Download from: https://developer.android.com/studio/releases/platform-tools" -ForegroundColor Yellow
    exit 1
}

# 检查设备连接
$deviceList = adb devices
if ($deviceList -match "device$") {
    Write-Host "设备连接正常" -ForegroundColor Green
} else {
    Write-Error "未找到连接的设备或设备未授权"
    Write-Host "请确保设备已通过 USB 连接并开启 ADB 调试" -ForegroundColor Yellow
    exit 1
}

Write-Host "开始部署 mxCamera..." -ForegroundColor Yellow

# 创建目标目录
Write-Host "创建目标目录..." -ForegroundColor Yellow
adb shell "mkdir -p /root/Workspace"

# 部署主程序
Write-Host "部署主程序..." -ForegroundColor Yellow
if (Test-Path "bin/mxCamera") {
    adb push bin/mxCamera /root/Workspace/mxCamera
    Write-Host "mxCamera 主程序部署完成" -ForegroundColor Green
    
    # 设置执行权限
    adb shell "chmod +x /root/Workspace/mxCamera"
} else {
    Write-Error "bin/mxCamera 文件不存在"
    exit 1
}

# 部署动态库文件
Write-Host "部署动态库文件..." -ForegroundColor Yellow
if (Test-Path "lib") {
    $libFiles = Get-ChildItem "lib" -Filter "*.so*"
    foreach ($libFile in $libFiles) {
        adb push $libFile.FullName "/usr/lib/$($libFile.Name)"
        Write-Host "部署库文件: $($libFile.Name)" -ForegroundColor Green
    }
} else {
    Write-Warning "lib 目录不存在，跳过库文件部署"
}

# 部署启动脚本
Write-Host "部署启动脚本..." -ForegroundColor Yellow
if (Test-Path "mxcamera") {
    adb push mxcamera /etc/init.d/S99mxcamera
    Write-Host "启动脚本部署完成" -ForegroundColor Green
    
    # 设置执行权限
    adb shell "chmod +x /etc/init.d/S99mxcamera"
} else {
    Write-Warning "mxcamera 启动脚本不存在，跳过"
}

# 禁用冲突的系统服务
Write-Host "禁用冲突服务..." -ForegroundColor Yellow
$servicesToDisable = @(
    "S50telnet",
    "S60micinit", 
    "S99hciinit",
    "S99python"
)

foreach ($service in $servicesToDisable) {
    $servicePath = "/etc/init.d/$service"
    $checkResult = adb shell "[ -f $servicePath ] && echo 'exists' || echo 'not_found'"
    if ($checkResult.Trim() -eq "exists") {
        adb shell "mv $servicePath ${servicePath}.disabled"
        Write-Host "已禁用服务: $service" -ForegroundColor Green
    } else {
        Write-Host "服务 $service 不存在，跳过" -ForegroundColor Yellow
    }
}

Write-Host "" -ForegroundColor White
Write-Host "=== 部署完成 ===" -ForegroundColor Green
Write-Host "可执行文件位置: /root/Workspace/mxCamera" -ForegroundColor White
Write-Host "启动脚本位置: /etc/init.d/S99mxcamera" -ForegroundColor White
Write-Host "" -ForegroundColor White
Write-Host "手动启动方式:" -ForegroundColor Yellow
Write-Host "  adb shell '/root/Workspace/mxCamera'" -ForegroundColor White
Write-Host "" -ForegroundColor White
Write-Host "服务启动方式:" -ForegroundColor Yellow  
Write-Host "  adb shell '/etc/init.d/S99mxcamera start'" -ForegroundColor White
Write-Host "" -ForegroundColor White
Write-Host "重启设备自动启动:" -ForegroundColor Yellow
Write-Host "  adb reboot" -ForegroundColor White