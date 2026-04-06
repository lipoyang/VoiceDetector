#pragma once
#include <stdint.h>

// AudioClassを使わないマイククラス
class DirectMic
{
public:
    bool begin();
    void start();
    void stop();
    void end();
    void loop();
    
    void (*onData)(int16_t *data);
    void (*onError)(int err);
};