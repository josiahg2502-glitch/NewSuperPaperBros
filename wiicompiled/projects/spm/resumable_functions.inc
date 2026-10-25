// Translated functions emitted resumable at their call sites (ResumableFunctionRewriter). Every function on
// a guest thread's call chain at a VI suspend must be listed, or saving is refused. save_state.cpp
// includes this file too.
0x80006124,  // __start
0x8001D9A0,  // main
0x801A87F0,  // DEMODoneRender
0x801A7BEC,  // wiiDvdCheckThread
0x80305374,  // fn_80305374 (thread entry, waits in OSReceiveMessage via fn_803053C8)
0x803053C8,  // fn_803053C8
0x8030862C,  // fn_8030862C (thread entry, waits in OSReceiveMessage)
