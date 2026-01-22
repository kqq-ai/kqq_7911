# 切换调试模式模式

```shell
搜索关键词
#define TEST_LOCOL_SERVER
```

版本号

```
LOCAL_VERSION_CODE
```


网络连接配置:
```sh
.app_wifi_config = {
```


## 烧录流程

1. 先按住，进入烧录模式
2. 终端 -> 执行任务 ->  选择cookie
3. 等待程序烧录，日志将输出`Start to download ......`


## mac 直接编译

ssh x@192.168.50.205

net use Z: \\192.168.50.152\Embedded 1111 /user:renran

Z: && cd 
.vscode\winmk.bat ac791n_cookie


## 问题记录


### 使用mac共享文件到win ssh编译


出现错误 
```sh 
make[1]: *** INTERNAL: readdir: Invalid argument. Stop.
这不是你代码编译报错，而是 make 在遍历目录（readdir）时，底层文件系统返回了 EINVAL（参数无效），导致 make 直接中止。
同时你这里的路径显示为：
Leaving directory 'Z:/kqq_7911/apps/ling_wl/board/wl82'
说明你是在 Windows 上通过映射盘 Z:（通常是网络盘/sshfs/samba/WSL/远程挂载） 进行构建；这类挂载/文件系统在某些情况下会对 readdir() 返回异常（目录项/属性不符合预期），触发 make 的 internal error。
```
解决方法为: 
把工程拷贝到本地磁盘（C: 或 D:）再编译（最稳，通常立刻解决）。
因此放弃mac编译，还是采用远程ssh使用

## 蓝牙部分:
7911
负责 BLE 配网广播/连接、BLE 扫描与 RSSI 测距、BLE 任务调度、GATT 通讯
按键/触摸


7016:
音频/LE Audio 侧的 BLE 广播与播放
上报电量/充电状态给 7911
播放/OTA/关闭模式：进入播放、OTA、关闭


## 音频播放:
7911 自己解码/播放音频，同时切换到 E 扬声器输出，并通过 UART/控制命令让 7016 进入对应的广播/播放状态（更像前级/广播控制）。