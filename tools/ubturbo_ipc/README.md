# UBTurbo IPC Python 客户端

该目录提供通过 Unix Domain Socket 调用 UBTurbo SMAP 能力的 Python 客户端。客户端默认连接
`/opt/ubturbo/ubturbo_ipc`，应在 UBTurbo 服务启动且 socket 就绪后使用。

## 使用方法

脚本只依赖 Python 标准库，可以单独复制或下载到任意目录直接执行：

```bash
python3 ubturbo_ipc.py start 0
python3 ubturbo_ipc.py is_running
python3 ubturbo_ipc.py --help
```

不带参数执行时也会打印完整命令说明。`start` 的 `pageType` 参数取值如下：

| 取值  | 页面模式 | 典型场景 |
| ----- | -------- | -------- |
| `0` | 4K       | 容器     |
| `1` | 2M       | 虚机     |
