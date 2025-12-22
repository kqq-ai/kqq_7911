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