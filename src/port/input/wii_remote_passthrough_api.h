#pragma once
#include <stdint.h>
#ifdef _WIN32
  #define WII_BT_CALL __cdecl
  #if defined(HOTD2_WII_USB_DLL_BUILD)
    #define WII_BT_API __declspec(dllexport)
  #else
    #define WII_BT_API
  #endif
#else
  #define WII_BT_CALL
  #define WII_BT_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define WII_BT_API_VERSION 2u
typedef struct WiiBtPlayerState {
    uint32_t struct_size;
    int32_t battery;
    uint16_t buttons;
    uint16_t pressed;
    uint8_t connected;
    uint8_t reserved;
    uint8_t tracking;
    uint8_t dots;
    double raw_x;
    double raw_y;
} WiiBtPlayerState;
typedef struct WiiBtSnapshot {
    uint32_t struct_size;
    uint32_t scanning;
    WiiBtPlayerState player[2];
    char message[512];
} WiiBtSnapshot;
WII_BT_API uint32_t WII_BT_CALL WiiBt_GetApiVersion(void);
/* Pass -1/-1 for automatic selection, or an explicit USB VID/PID pair. */
WII_BT_API int WII_BT_CALL WiiBt_HasCompatibleAdapter(int32_t vid, int32_t pid);
WII_BT_API void* WII_BT_CALL WiiBt_Create(int32_t vid, int32_t pid);
WII_BT_API void WII_BT_CALL WiiBt_Destroy(void* instance);
WII_BT_API int WII_BT_CALL WiiBt_Enable(void* instance, int enabled, int continuous);
WII_BT_API int WII_BT_CALL WiiBt_Scan(void* instance);
WII_BT_API int WII_BT_CALL WiiBt_Disconnect(void* instance, int player);
WII_BT_API int WII_BT_CALL WiiBt_SwapPlayers(void* instance);
WII_BT_API int WII_BT_CALL WiiBt_Poll(void* instance, WiiBtSnapshot* snapshot);
#ifdef __cplusplus
}
#endif
