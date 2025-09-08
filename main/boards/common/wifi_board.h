#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"

class WifiBoard : public Board {
protected:
    bool wifi_config_mode_ = false;
    void EnterWifiConfigMode();
    std::string SendAndroidRequest(const std::string &text);
    virtual std::string GetBoardJson() override;

public:
    WifiBoard();
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual void ResetWifiConfiguration();
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;

    // 实现一个函数来控制安卓显示表情 
    virtual void ShowAndroidEmoji(const std::string &emoji_name) override;
    // 实现一个函数来控制安卓显示文本
    virtual void ShowAndroidText(const std::string &text) override;
    // 实现一个函数来控制安卓拍照并上传服务器
    virtual std::string ShowAndroidTakePhoto() override;
};

#endif // WIFI_BOARD_H
