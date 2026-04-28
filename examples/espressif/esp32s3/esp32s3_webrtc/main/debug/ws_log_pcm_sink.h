#ifndef WS_LOG_PCM_SINK_H
#define WS_LOG_PCM_SINK_H

#include <cstddef>
#include <cstdint>
#include <string>

// 通过独立 WebSocket 发送 ESP 日志行（文本 JSON）与 PCM 二进制帧；与业务协议 WebSocket 分离。
class WsLogPcmSink {
public:
    static WsLogPcmSink& Instance();

    // 从 NVS 命名空间 "ws_debug" 读取 url，为空则使用 Kconfig CONFIG_WS_DEBUG_DEFAULT_URL
    void ConfigureFromNvs();
    // 网络就绪后调用：安装日志钩子、启动发送任务并连接（url 为空则无效）
    void StartIfEnabled();
    void Stop();

    // 非阻塞入队；队列满则丢弃
    void EnqueueLogLine(const char* line);
    void EnqueuePcm(uint32_t sample_rate, uint16_t channels, const int16_t* samples, size_t sample_count);

    bool IsEnabled() const { return enabled_; }

private:
    WsLogPcmSink() = default;
    WsLogPcmSink(const WsLogPcmSink&) = delete;
    WsLogPcmSink& operator=(const WsLogPcmSink&) = delete;

    std::string url_;
    bool enabled_ = false;
    bool started_ = false;
};

#endif
