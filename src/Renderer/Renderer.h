#pragma once

#include "Defines.h"

bool init_renderer();
bool create_pipeline();
void kill_renderer();

bool renderer_draw_frame();
bool renderer_present();