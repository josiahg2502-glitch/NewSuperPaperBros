#pragma once
// Only the owning frontend thread may reset HLE state, after fibers and workers stop.
void VI_HLE_ResetFrontend();
void DVD_HLE_ResetFrontend();
void NAND_HLE_ResetFrontendFiles();
void NAND_HLE_ResetFrontendCallbacks();
void NAND_HLE_ResetFrontendIos();
void OS_HLE_ResetFrontendSleep();
void WPAD_HLE_ResetFrontend();
void AI_HLE_ResetFrontend();
