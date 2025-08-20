
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

# 部署图标文件
Write-Host "部署图标文件..." -ForegroundColor Yellow
if (Test-Path "icon") {
    # 创建图标目录
    adb shell "mkdir -p /root/Workspace/icon"
    
    # 部署所有图标文件
    $iconFiles = Get-ChildItem "icon" -Filter "*.png"
    if ($iconFiles.Count -gt 0) {
        foreach ($iconFile in $iconFiles) {
            adb push $iconFile.FullName "/root/Workspace/icon/$($iconFile.Name)"
            Write-Host "部署图标文件: $($iconFile.Name)" -ForegroundColor Green
        }
        Write-Host "图标文件部署完成 ($($iconFiles.Count) 个文件)" -ForegroundColor Green
    } else {
        Write-Warning "icon 目录中没有找到 PNG 文件"
    }
} else {
    Write-Warning "icon 目录不存在，跳过图标文件部署"
}

# 部署动态库文件
Write-Host "部署动态库文件..." -ForegroundColor Yellow
if (Test-Path "lib") {
    # 首先部署所有库文件
    $libFiles = Get-ChildItem "lib" -Filter "*.so*"
    foreach ($libFile in $libFiles) {
        adb push $libFile.FullName "/usr/lib/$($libFile.Name)"
        Write-Host "部署库文件: $($libFile.Name)" -ForegroundColor Green
    }
    
    # 创建软符号连接
    Write-Host "创建软符号连接..." -ForegroundColor Yellow
    
    # 自动识别版本化的库文件并创建软链接
    $versionedLibs = Get-ChildItem "lib" -Filter "*.so.*.*.*" | Where-Object { $_.Name -match '\.so\.\d+\.\d+\.\d+$' }
    
    foreach ($versionedLib in $versionedLibs) {
        $realLibName = $versionedLib.Name
        $libPath = "/usr/lib/$realLibName"
        
        # 检查实际库文件是否存在
        $checkResult = adb shell "[ -f $libPath ] && echo 'exists' || echo 'not_found'"
        if ($checkResult.Trim() -eq "exists") {
            # 解析库名和版本号
            if ($realLibName -match '^(.+)\.so\.(\d+)\.(\d+)\.(\d+)$') {
                $baseName = $matches[1]
                $majorVersion = $matches[2]
                $minorVersion = $matches[3]
                $patchVersion = $matches[4]
                
                # 创建软链接: libname.so.major -> libname.so.major.minor.patch
                $majorLink = "$baseName.so.$majorVersion"
                $majorLinkPath = "/usr/lib/$majorLink"
                adb shell "rm -f $majorLinkPath"
                adb shell "ln -s $realLibName $majorLinkPath"
                Write-Host "创建软链接: $majorLink -> $realLibName" -ForegroundColor Green
                
                # 创建软链接: libname.so -> libname.so.major.minor.patch
                $baseLink = "$baseName.so"
                $baseLinkPath = "/usr/lib/$baseLink"
                adb shell "rm -f $baseLinkPath"
                adb shell "ln -s $realLibName $baseLinkPath"
                Write-Host "创建软链接: $baseLink -> $realLibName" -ForegroundColor Green
            } else {
                Write-Warning "无法解析库文件版本号: $realLibName"
            }
        } else {
            Write-Warning "库文件 $realLibName 不存在，跳过软链接创建"
        }
    }
} else {
    Write-Warning "lib 目录不存在，跳过库文件部署"
}

# 部署配置文件
Write-Host "部署配置文件..." -ForegroundColor Yellow
if (Test-Path "mxCamera_config_example.toml") {
    adb push mxCamera_config_example.toml /root/Workspace/mxCamera_config.toml
    Write-Host "配置文件部署完成: mxCamera_config.toml" -ForegroundColor Green
} else {
    Write-Warning "mxCamera_config_example.toml 不存在，跳过配置文件部署"
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
    "S99python",
    "S99usb0config"
)

adb shell "mkdir -p /etc/init_d_backup/"

foreach ($service in $servicesToDisable) {
    $servicePath = "/etc/init.d/$service"
    $checkResult = adb shell "[ -f $servicePath ] && echo 'exists' || echo 'not_found'"
    if ($checkResult.Trim() -eq "exists") {
        adb shell "mv $servicePath /etc/init_d_backup/${service}"
        Write-Host "已禁用服务: $service" -ForegroundColor Green
    } else {
        Write-Host "服务 $service 不存在，跳过" -ForegroundColor Yellow
    }
}

Write-Host "" -ForegroundColor White
Write-Host "=== 部署完成 ===" -ForegroundColor Green
Write-Host "可执行文件位置: /root/Workspace/mxCamera" -ForegroundColor White
Write-Host "配置文件位置: /root/Workspace/mxCamera_config.toml" -ForegroundColor White
Write-Host "图标文件位置: /root/Workspace/icon/*.png" -ForegroundColor White
Write-Host "启动脚本位置: /etc/init.d/S99mxcamera" -ForegroundColor White
Write-Host "" -ForegroundColor White
Write-Host "手动启动方式:" -ForegroundColor Yellow
Write-Host "  adb shell 'cd /root/Workspace && ./mxCamera'" -ForegroundColor White
Write-Host "" -ForegroundColor White
Write-Host "服务启动方式:" -ForegroundColor Yellow  
Write-Host "  adb shell '/etc/init.d/S99mxcamera start'" -ForegroundColor White
Write-Host "" -ForegroundColor White
Write-Host "重启设备自动启动:" -ForegroundColor Yellow
Write-Host "  adb reboot" -ForegroundColor White
Write-Host "" -ForegroundColor White
Write-Host "配置管理:" -ForegroundColor Yellow
Write-Host "  应用程序会自动读取并保存配置到 /root/Workspace/mxCamera_config.toml" -ForegroundColor White
Write-Host "  图标文件位于 /root/Workspace/icon/ 目录，用于显示设备状态" -ForegroundColor White
Write-Host "  如果图标文件缺失，程序会自动使用文字显示作为后备方案" -ForegroundColor White
Write-Host "  可以直接编辑配置文件来调整摄像头参数" -ForegroundColor White