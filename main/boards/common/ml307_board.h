#ifndef ML307_BOARD_H
#define ML307_BOARD_H

#include <memory>
#include <at_modem.h>
#include "board.h"


class Ml307Board : public Board {
protected:
    std::unique_ptr<AtModem> modem_;
    gpio_num_t tx_pin_;
    gpio_num_t rx_pin_;
    gpio_num_t dtr_pin_;
    std::string SendAndroidRequest(const std::string &text);
    virtual std::string GetBoardJson() override;

public:
    Ml307Board(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin = GPIO_NUM_NC);
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
    // 实现一个函数来控制安卓显示表情 
    virtual void ShowAndroidEmoji(const std::string &emoji_name) override;
    // 实现一个函数来控制安卓显示文本
    virtual void ShowAndroidText(const std::string &text) override;
    // 实现一个函数来控制安卓拍照并上传服务器
    virtual std::string ShowAndroidTakePhoto() override;};

#endif // ML307_BOARD_H
