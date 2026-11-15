#pragma once
// LovyanGFX composite video (CVBS) output via the ESP32 internal DAC.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "config.h"

class LGFX_CVBS : public lgfx::LGFX_Device
{
  public:
    lgfx::Panel_CVBS _panel_instance;

    LGFX_CVBS(void)
    {
        {
            auto cfg = _panel_instance.config();
            cfg.memory_width = CVBS_MEMORY_WIDTH;
            cfg.memory_height = CVBS_MEMORY_HEIGHT;
            cfg.panel_width = CVBS_PANEL_WIDTH;
            cfg.panel_height = CVBS_PANEL_HEIGHT;
            cfg.offset_x = CVBS_OFFSET_X;
            cfg.offset_y = CVBS_OFFSET_Y;
            _panel_instance.config(cfg);
        }
        {
            auto cfg = _panel_instance.config_detail();
#if CVBS_PAL
            cfg.signal_type = cfg.signal_type_t::PAL;
#else
            cfg.signal_type = cfg.signal_type_t::NTSC;
#endif
            cfg.pin_dac = CVBS_PIN;
            cfg.use_psram = 0;
            cfg.output_level = CVBS_OUTPUT_LEVEL;
            cfg.chroma_level = CVBS_CHROMA_LEVEL;
            _panel_instance.config_detail(cfg);
        }
#if CVBS_GRAYSCALE
        // Before init(), so the frame buffer is allocated only once
        _panel_instance.setColorDepth(lgfx::color_depth_t::grayscale_8bit);
#endif
        setPanel(&_panel_instance);
    }
};
