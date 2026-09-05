#pragma once

#include <cstddef>

namespace kmmo {

// Start the login/register panel window on its own thread.
void PanelStart(const char* serverHost, int serverPort);

// Stop the panel thread and destroy the window.
void PanelStop();

} // namespace kmmo