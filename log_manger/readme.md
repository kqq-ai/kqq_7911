## log_manger：串口日志局域网实时查看（SSE）

### 一键启动（已内置默认参数）

在仓库根目录 PowerShell：

```powershell
python log_manger/serial_sse_logger.py
```

默认等价于你以前的命令：

- `--port COM6`
- `--baud 1000000`
- `--out .\uart.log`
- 自动重连开启
- `--flush-interval 0`（每行落盘）

### 局域网访问

- 浏览器：`http://抓日志电脑IP:8000/`
- SSE：`http://抓日志电脑IP:8000/events`
- 健康检查：`http://抓日志电脑IP:8000/health`

