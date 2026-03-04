/*
 * 飞书 (Feishu) 机器人配置示例
 * 需要飞书开发者账号和应用
 * 
 * 1. 在飞书开发者后台创建企业自建应用
 * 2. 获取App ID 和 App Secret
 * 3. 通过串口设置:
 *    set_feishu_app_id cli_xxxxxxxxxxxx
 *    set_feishu_app_secret xxxxxxxxxxxxxxxxxxxxxxx
 * 
 * 示例：
 * - 用户在飞书中发送 "你好" 
 * - 飞书服务接收到信息(此部分需外部实现-webhook或polling)
 * - 传递给MimiClaw的消息总线进行AI处理
 * - AI回复通过feishu_send_message发送回去
 */

// 示例配置
#define EXAMPLE_FEISHU_APP_ID "cli_xxxxxxxxxxxx"
#define EXAMPLE_FEISHU_APP_SECRET "xxxxxxxxxxxxxxxxxxxxxxxxx"

// 注册飞书通道的示例代码（已在主程序中完成）
extern esp_err_t register_feishu_channel() {
    extern const void* feishu_get_channel_interface(void);
    return channel_register((const channel_interface_t *)feishu_get_channel_interface());
}