#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

#include <string>

/*** pre环境 ***/
/*
// Activate API base URL
inline const std::string kActivateApiBase = "https://baiyu-pre.100credit.cn/api/hardware/smartvoice";
// RTC / realtime Voice URL
inline const std::string kRtcVoiceUrl = "https://baiyu-pre.100credit.cn/api/realtime/webrtc/v1";
// MQTT broker endpoint (host:port)
inline const std::string kMqttBrokerEndpoint = "baiyu-link-pre.100credit.cn:8883";
*/
/*** 生产环境 ***/

// Activate API base URL
inline const std::string kActivateApiBase = "https://baiyu.resultscloud.com/api/hardware/smartvoice";
// RTC / realtime Voice URL
inline const std::string kRtcVoiceUrl = "https://baiyu.resultscloud.com/api/realtime/webrtc/v1";
// MQTT broker endpoint (host:port)
inline const std::string kMqttBrokerEndpoint = "baiyu-link.resultscloud.com:8883";


#endif  // APP_CONFIG_H_
