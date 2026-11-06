// The wheel probe on its own (fm4.exe --wheel-probe without the game): for testing the racing wheel
// code and the launcher's Wheel stuff tab on a PC without a recompiled game. FM4_WHEEL_PROBE=<this
// exe> makes the launcher use it.
#include "input/wheel/wheel_system.h"

int main() { return wheel::ProbeMain(); }
