#ifndef ALARM_MCP_TOOLS_H
#define ALARM_MCP_TOOLS_H

#include "mcp_server.h"
#include "alarm_manager.h"
#include <memory>

// 闹钟MCP工具类 - 将闹钟功能暴露给AI助手
class AlarmMcpTools {
public:
    // 注册所有闹钟相关的MCP工具
    static void RegisterTools(McpServer& server);

private:
    // 设置闹钟工具
    static ReturnValue SetAlarmTool(const PropertyList& properties);
    
    // 列出闹钟工具
    static ReturnValue ListAlarmsTool(const PropertyList& properties);
    
    // 取消闹钟工具
    static ReturnValue CancelAlarmTool(const PropertyList& properties);
    
    // 获取当前时间工具
    static ReturnValue GetCurrentTimeTool(const PropertyList& properties);
    
    // 设置闹钟声音工具
    static ReturnValue SetAlarmSoundTool(const PropertyList& properties);
    
    // 获取闹钟声音类型工具
    static ReturnValue GetAlarmSoundTypesTool(const PropertyList& properties);
    
    // 获取默认闹钟声音类型工具
    static ReturnValue GetDefaultSoundTypeTool(const PropertyList& properties);
    
    // 格式化闹钟信息为JSON字符串
    static std::string FormatAlarmInfoJson(const AlarmInfo& alarm);
    
    // 格式化闹钟列表为JSON字符串
    static std::string FormatAlarmListJson(const std::vector<AlarmInfo>& alarms);
};

#endif // ALARM_MCP_TOOLS_H 