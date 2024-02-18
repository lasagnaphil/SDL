/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_GAMEINPUT

#include "../SDL_sysjoystick.h"
#include "../usb_ids.h"

#include <stdbool.h>
#define COBJMACROS
#include <GameInput.h>


typedef struct GAMEINPUT_InternalDevice
{
    IGameInputDevice *device;
    const char *deviceName;        /* this is a constant string literal */
    Uint16 vendor;
    Uint16 product;
    SDL_JoystickGUID joystickGuid; /* generated by SDL. */
    SDL_JoystickID instanceId;     /* generated by SDL. */
    int playerIndex;
    GameInputRumbleMotors supportedRumbleMotors;
    char devicePath[(APP_LOCAL_DEVICE_ID_SIZE * 2) + 1];
    SDL_bool isAdded, isDeleteRequested;
} GAMEINPUT_InternalDevice;

typedef struct GAMEINPUT_InternalList
{
    GAMEINPUT_InternalDevice **devices;
    int count;
} GAMEINPUT_InternalList;

typedef struct joystick_hwdata
{
    GAMEINPUT_InternalDevice *devref;
    GameInputRumbleParams rumbleParams;
    Uint64 lastTimestamp;
} GAMEINPUT_InternalJoystickHwdata;


static GAMEINPUT_InternalList g_GameInputList = { NULL };
static void *g_hGameInputDLL = NULL;
static IGameInput *g_pGameInput = NULL;
static GameInputCallbackToken g_GameInputCallbackToken = GAMEINPUT_INVALID_CALLBACK_TOKEN_VALUE;


static int GAMEINPUT_InternalAddOrFind(IGameInputDevice *pDevice)
{
    GAMEINPUT_InternalDevice **devicelist = NULL;
    GAMEINPUT_InternalDevice *elem = NULL;
    const GameInputDeviceInfo *devinfo = NULL;
    Uint16 bus = SDL_HARDWARE_BUS_USB;
    Uint16 vendor = 0;
    Uint16 product = 0;
    Uint16 version = 0;
    char tmpbuff[4];
    int idx = 0;

    if (!pDevice) {
        return SDL_SetError("GAMEINPUT_InternalAddOrFind argument pDevice cannot be NULL");
    }

    devinfo = IGameInputDevice_GetDeviceInfo(pDevice);
    if (!devinfo) {
        return SDL_SetError("GAMEINPUT_InternalAddOrFind GetDeviceInfo returned NULL");
    }

    for (idx = 0; idx < g_GameInputList.count; ++idx) {
        elem = g_GameInputList.devices[idx];
        if (elem && elem->device == pDevice) {
            /* we're already added */
            return idx;
        }
    }

    elem = (GAMEINPUT_InternalDevice *)SDL_calloc(1, sizeof(*elem));
    if (!elem) {
        return SDL_OutOfMemory();
    }

    devicelist = (GAMEINPUT_InternalDevice **)SDL_realloc(g_GameInputList.devices, sizeof(elem) * (g_GameInputList.count + 1LL));
    if (!devicelist) {
        SDL_free(elem);
        return SDL_OutOfMemory();
    }

    /* generate a device name */
    for (idx = 0; idx < APP_LOCAL_DEVICE_ID_SIZE; ++idx) {
        SDL_snprintf(tmpbuff, SDL_arraysize(tmpbuff), "%02hhX", devinfo->deviceId.value[idx]);
        SDL_strlcat(elem->devicePath, tmpbuff, SDL_arraysize(tmpbuff));
    }
    if (devinfo->capabilities & GameInputDeviceCapabilityWireless) {
        bus = SDL_HARDWARE_BUS_BLUETOOTH;
    } else {
        bus = SDL_HARDWARE_BUS_USB;
    }
    vendor = devinfo->vendorId;
    product = devinfo->productId;
    version = (devinfo->firmwareVersion.major << 8) | devinfo->firmwareVersion.minor;

    g_GameInputList.devices = devicelist;
    IGameInputDevice_AddRef(pDevice);
    elem->device = pDevice;
    elem->deviceName = "GameInput Gamepad";
    elem->vendor = vendor;
    elem->product = product;
    elem->supportedRumbleMotors = devinfo->supportedRumbleMotors;
    elem->joystickGuid = SDL_CreateJoystickGUID(bus, vendor, product, version, "GameInput", "Gamepad", 'g', 0);
    elem->instanceId = SDL_GetNextObjectID();
    g_GameInputList.devices[g_GameInputList.count] = elem;

    /* finally increment the count and return */
    return g_GameInputList.count++;
}

static int GAMEINPUT_InternalRemoveByIndex(int idx)
{
    GAMEINPUT_InternalDevice **devicelist = NULL;
    int bytes = 0;

    if (idx < 0 || idx >= g_GameInputList.count) {
        return SDL_SetError("GAMEINPUT_InternalRemoveByIndex argument idx %d is out of range", idx);
    }

    IGameInputDevice_Release(g_GameInputList.devices[idx]->device);

    if (g_GameInputList.devices[idx]) {
        SDL_free(g_GameInputList.devices[idx]);
        g_GameInputList.devices[idx] = NULL;
    }

    if (g_GameInputList.count == 1) {
        /* last element in the list, free the entire list then */
        SDL_free(g_GameInputList.devices);
        g_GameInputList.devices = NULL;
    } else {
        if (idx != g_GameInputList.count - 1) {
            bytes = sizeof(*devicelist) * (g_GameInputList.count - idx);
            SDL_memmove(&g_GameInputList.devices[idx], &g_GameInputList.devices[idx + 1], bytes);
        }

        devicelist = (GAMEINPUT_InternalDevice **)SDL_realloc(g_GameInputList.devices, sizeof(*devicelist) * (g_GameInputList.count - 1LL));
        if (!devicelist) {
            return SDL_OutOfMemory();
        }

        g_GameInputList.devices = devicelist;
    }

    /* decrement the count and return */
    return g_GameInputList.count--;
}

static GAMEINPUT_InternalDevice *GAMEINPUT_InternalFindByIndex(int idx)
{
    if (idx < 0 || idx >= g_GameInputList.count) {
        SDL_SetError("GAMEINPUT_InternalFindByIndex argument idx %d out of range", idx);
        return NULL;
    }

    return g_GameInputList.devices[idx];
}

static void CALLBACK GAMEINPUT_InternalJoystickDeviceCallback(
    _In_ GameInputCallbackToken callbackToken,
    _In_ void* context,
    _In_ IGameInputDevice* device,
    _In_ uint64_t timestamp,
    _In_ GameInputDeviceStatus currentStatus,
    _In_ GameInputDeviceStatus previousStatus)
{
    int idx = 0;
    GAMEINPUT_InternalDevice *elem = NULL;

    if (currentStatus & GameInputDeviceConnected) {
        GAMEINPUT_InternalAddOrFind(device);
    } else {
        for (idx = 0; idx < g_GameInputList.count; ++idx) {
            elem = g_GameInputList.devices[idx];
            if (elem && elem->device == device) {
                /* will be deleted on the next Detect call */
                elem->isDeleteRequested = SDL_TRUE;
                break;
            }
        }
    }
}

static void GAMEINPUT_JoystickDetect(void);

static int GAMEINPUT_JoystickInit(void)
{
    HRESULT hR;

    if (!g_hGameInputDLL) {
        g_hGameInputDLL = SDL_LoadObject("gameinput.dll");
        if (!g_hGameInputDLL) {
            return -1;
        }
    }

    if (!g_pGameInput) {
        typedef HRESULT (WINAPI *GameInputCreate_t)(IGameInput * *gameInput);
        GameInputCreate_t GameInputCreateFunc = (GameInputCreate_t)SDL_LoadFunction(g_hGameInputDLL, "GameInputCreate");
        if (!GameInputCreateFunc) {
            return -1;
        }

        hR = GameInputCreateFunc(&g_pGameInput);
        if (FAILED(hR)) {
            return SDL_SetError("GameInputCreate failure with HRESULT of %08X", hR);
        }
    }

    hR = IGameInput_RegisterDeviceCallback(g_pGameInput,
                                           NULL,
                                           GameInputKindGamepad,
                                           GameInputDeviceConnected,
                                           GameInputBlockingEnumeration,
                                           NULL,
                                           GAMEINPUT_InternalJoystickDeviceCallback,
                                           &g_GameInputCallbackToken);
    if (FAILED(hR)) {
        return SDL_SetError("IGameInput::RegisterDeviceCallback failure with HRESULT of %08X", hR);
    }

    GAMEINPUT_JoystickDetect();

    return 0;
}

static int GAMEINPUT_JoystickGetCount(void)
{
    return g_GameInputList.count;
}

static void GAMEINPUT_JoystickDetect(void)
{
    int idx = 0;
    GAMEINPUT_InternalDevice *elem = NULL;

    for (idx = 0; idx < g_GameInputList.count; ++idx) {
        elem = g_GameInputList.devices[idx];
        if (!elem) {
            continue;
        }

        if (!elem->isAdded) {
            SDL_PrivateJoystickAdded(elem->instanceId);
            elem->isAdded = SDL_TRUE;
        }

        if (elem->isDeleteRequested || !(IGameInputDevice_GetDeviceStatus(elem->device) & GameInputDeviceConnected)) {
            SDL_PrivateJoystickRemoved(elem->instanceId);
            GAMEINPUT_InternalRemoveByIndex(idx--);
        }
    }
}

static SDL_bool GAMEINPUT_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    int idx = 0;
    GAMEINPUT_InternalDevice *elem = NULL;

    if (vendor_id == USB_VENDOR_MICROSOFT &&
        product_id == USB_PRODUCT_XBOX_ONE_XBOXGIP_CONTROLLER) {
        /* The Xbox One controller shows up as a hardcoded raw input VID/PID, which we definitely handle */
        return SDL_TRUE;
    }

    for (idx = 0; idx < g_GameInputList.count; ++idx) {
        elem = g_GameInputList.devices[idx];
        if (elem && vendor_id == elem->vendor && product_id == elem->product) {
            return SDL_TRUE;
        }
    }
    return SDL_FALSE;
}

static const char *GAMEINPUT_JoystickGetDeviceName(int device_index)
{
    GAMEINPUT_InternalDevice *elem = GAMEINPUT_InternalFindByIndex(device_index);

    if (!elem) {
        return NULL;
    }

    return elem->deviceName;
}

static const char *GAMEINPUT_JoystickGetDevicePath(int device_index)
{
    GAMEINPUT_InternalDevice *elem = GAMEINPUT_InternalFindByIndex(device_index);

    if (!elem) {
        return NULL;
    }

    /* APP_LOCAL_DEVICE_ID as a hex string, since it's required for some association callbacks */
    return elem->devicePath;
}

static int GAMEINPUT_JoystickGetDeviceSteamVirtualGamepadSlot(int device_index)
{
    /* Steamworks API is not available in GDK */
    return -1;
}

static int GAMEINPUT_JoystickGetDevicePlayerIndex(int device_index)
{
    /*
     * Okay, so, while XInput technically has player indicies,
     * GameInput does not. It just dispatches a callback whenever a device is found.
     * So if you're using true native GameInput (which this backend IS)
     * you're meant to assign some index to a player yourself.
     *
     * GameMaker, for example, seems to do this in the order of plugging in.
     *
     * Sorry for the trouble!
     */
    GAMEINPUT_InternalDevice *elem = GAMEINPUT_InternalFindByIndex(device_index);

    if (!elem) {
        return -1;
    }

    return elem->playerIndex;
}

static void GAMEINPUT_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
    GAMEINPUT_InternalDevice *elem = GAMEINPUT_InternalFindByIndex(device_index);

    if (!elem) {
        return;
    }

    elem->playerIndex = player_index;
}

static SDL_JoystickGUID GAMEINPUT_JoystickGetDeviceGUID(int device_index)
{
    GAMEINPUT_InternalDevice *elem = GAMEINPUT_InternalFindByIndex(device_index);

    if (!elem) {
        static SDL_JoystickGUID emptyGUID;
        return emptyGUID;
    }

    return elem->joystickGuid;
}

static SDL_JoystickID GAMEINPUT_JoystickGetDeviceInstanceID(int device_index)
{
    GAMEINPUT_InternalDevice *elem = GAMEINPUT_InternalFindByIndex(device_index);

    if (!elem) {
        return 0;
    }

    return elem->instanceId;
}

static int GAMEINPUT_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    GAMEINPUT_InternalDevice *elem = GAMEINPUT_InternalFindByIndex(device_index);
    GAMEINPUT_InternalJoystickHwdata *hwdata = NULL;

    if (!elem) {
        return -1;
    }

    hwdata = (GAMEINPUT_InternalJoystickHwdata *)SDL_calloc(1, sizeof(*hwdata));
    if (!hwdata) {
        return SDL_OutOfMemory();
    }

    hwdata->devref = elem;

    joystick->hwdata = hwdata;
    joystick->naxes = 6;
    joystick->nbuttons = 11;
    joystick->nhats = 1;

    if (elem->supportedRumbleMotors & (GameInputRumbleLowFrequency | GameInputRumbleHighFrequency)) {
        SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, SDL_TRUE);
    }
    if (elem->supportedRumbleMotors & (GameInputRumbleLeftTrigger | GameInputRumbleRightTrigger)) {
        SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), SDL_PROP_JOYSTICK_CAP_TRIGGER_RUMBLE_BOOLEAN, SDL_TRUE);
    }

    return 0;
}

static int GAMEINPUT_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    /* don't check for caps here, since SetRumbleState doesn't return any result - we don't need to check it */
    GAMEINPUT_InternalJoystickHwdata *hwdata = joystick->hwdata;
    GameInputRumbleParams *params = &hwdata->rumbleParams;
    params->lowFrequency = (float)low_frequency_rumble / (float)SDL_MAX_UINT16;
    params->highFrequency = (float)high_frequency_rumble / (float)SDL_MAX_UINT16;
    IGameInputDevice_SetRumbleState(hwdata->devref->device, params);
    return 0;
}

static int GAMEINPUT_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    /* don't check for caps here, since SetRumbleState doesn't return any result - we don't need to check it */
    GAMEINPUT_InternalJoystickHwdata *hwdata = joystick->hwdata;
    GameInputRumbleParams *params = &hwdata->rumbleParams;
    params->leftTrigger = (float)left_rumble / (float)SDL_MAX_UINT16;
    params->rightTrigger = (float)right_rumble / (float)SDL_MAX_UINT16;
    IGameInputDevice_SetRumbleState(hwdata->devref->device, params);
    return 0;
}

static int GAMEINPUT_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static int GAMEINPUT_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static int GAMEINPUT_JoystickSetSensorsEnabled(SDL_Joystick *joystick, SDL_bool enabled)
{
    /* I am not sure what is this even supposed to do in case of GameInput... */
    return 0;
}

static void GAMEINPUT_JoystickUpdate(SDL_Joystick *joystick)
{
    static WORD s_XInputButtons[] = {
        GameInputGamepadA, GameInputGamepadB, GameInputGamepadX, GameInputGamepadY,
        GameInputGamepadLeftShoulder, GameInputGamepadRightShoulder, GameInputGamepadView, GameInputGamepadMenu,
        GameInputGamepadLeftThumbstick, GameInputGamepadRightThumbstick,
        0 /* Guide button is not supported on Xbox so ignore that... */
    };
    Uint8 btnidx = 0, btnstate = 0, hat = 0;
    GAMEINPUT_InternalJoystickHwdata *hwdata = joystick->hwdata;
    IGameInputDevice *device = hwdata->devref->device;
    IGameInputReading *reading = NULL;
    uint64_t ts = 0;
    GameInputGamepadState state;
    HRESULT hR = IGameInput_GetCurrentReading(g_pGameInput,
        GameInputKindGamepad,
        device,
        &reading
    );

    if (FAILED(hR)) {
        /* don't SetError here since there can be a legitimate case when there's no reading avail */
        return;
    }

    /* GDKX private docs for GetTimestamp: "The microsecond timestamp describing when the input was made." */
    /* SDL expects a nanosecond timestamp, so I guess US_TO_NS should be used here? */
    ts = SDL_US_TO_NS(IGameInputReading_GetTimestamp(reading));

    if (((!hwdata->lastTimestamp) || (ts != hwdata->lastTimestamp)) && IGameInputReading_GetGamepadState(reading, &state)) {
        /* `state` is now valid */

#define tosint16(_TheValue) ((Sint16)(((_TheValue) < 0.0f) ? ((_TheValue) * 32768.0f) : ((_TheValue) * 32767.0f)))
        SDL_SendJoystickAxis(ts, joystick, 0, tosint16(state.leftThumbstickX));
        SDL_SendJoystickAxis(ts, joystick, 1, tosint16(state.leftThumbstickY));
        SDL_SendJoystickAxis(ts, joystick, 2, tosint16(state.leftTrigger));
        SDL_SendJoystickAxis(ts, joystick, 3, tosint16(state.rightThumbstickX));
        SDL_SendJoystickAxis(ts, joystick, 4, tosint16(state.rightThumbstickY));
        SDL_SendJoystickAxis(ts, joystick, 5, tosint16(state.rightTrigger));
#undef tosint16

        for (btnidx = 0; btnidx < (Uint8)SDL_arraysize(s_XInputButtons); ++btnidx) {
            if (s_XInputButtons[btnidx] == 0) {
                btnstate = SDL_RELEASED;
            } else {
                btnstate = (state.buttons & s_XInputButtons[btnidx]) ? SDL_PRESSED : SDL_RELEASED;
            }

            SDL_SendJoystickButton(ts, joystick, btnidx, btnstate);
        }

        if (state.buttons & GameInputGamepadDPadUp) {
            hat |= SDL_HAT_UP;
        }
        if (state.buttons & GameInputGamepadDPadDown) {
            hat |= SDL_HAT_DOWN;
        }
        if (state.buttons & GameInputGamepadDPadLeft) {
            hat |= SDL_HAT_LEFT;
        }
        if (state.buttons & GameInputGamepadDPadRight) {
            hat |= SDL_HAT_RIGHT;
        }
        SDL_SendJoystickHat(ts, joystick, 0, hat);

        /* Xbox doesn't let you obtain the power level, pretend we're always full */
        SDL_SendJoystickBatteryLevel(joystick, SDL_JOYSTICK_POWER_FULL);

        hwdata->lastTimestamp = ts;
    }

    IGameInputReading_Release(reading);
}

static void GAMEINPUT_JoystickClose(SDL_Joystick* joystick)
{
    SDL_free(joystick->hwdata);
    joystick->hwdata = NULL;
}

static void GAMEINPUT_JoystickQuit(void)
{
    int idx;

    if (g_pGameInput) {
        /* free the callback */
        IGameInput_UnregisterCallback(g_pGameInput, g_GameInputCallbackToken, /*timeoutInUs:*/ 10000);
        g_GameInputCallbackToken = GAMEINPUT_INVALID_CALLBACK_TOKEN_VALUE;

        /* free the list */
        for (idx = 0; idx < g_GameInputList.count; ++idx) {
            IGameInputDevice_Release(g_GameInputList.devices[idx]->device);
            SDL_free(g_GameInputList.devices[idx]);
            g_GameInputList.devices[idx] = NULL;
        }
        SDL_free(g_GameInputList.devices);
        g_GameInputList.devices = NULL;
        g_GameInputList.count = 0;

        IGameInput_Release(g_pGameInput);
        g_pGameInput = NULL;
    }

    if (g_hGameInputDLL) {
        SDL_UnloadObject(g_hGameInputDLL);
        g_hGameInputDLL = NULL;
    }
}

static SDL_bool GAMEINPUT_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    return SDL_FALSE;
}


SDL_JoystickDriver SDL_GAMEINPUT_JoystickDriver =
{
    GAMEINPUT_JoystickInit,
    GAMEINPUT_JoystickGetCount,
    GAMEINPUT_JoystickDetect,
    GAMEINPUT_JoystickIsDevicePresent,
    GAMEINPUT_JoystickGetDeviceName,
    GAMEINPUT_JoystickGetDevicePath,
    GAMEINPUT_JoystickGetDeviceSteamVirtualGamepadSlot,
    GAMEINPUT_JoystickGetDevicePlayerIndex,
    GAMEINPUT_JoystickSetDevicePlayerIndex,
    GAMEINPUT_JoystickGetDeviceGUID,
    GAMEINPUT_JoystickGetDeviceInstanceID,
    GAMEINPUT_JoystickOpen,
    GAMEINPUT_JoystickRumble,
    GAMEINPUT_JoystickRumbleTriggers,
    GAMEINPUT_JoystickSetLED,
    GAMEINPUT_JoystickSendEffect,
    GAMEINPUT_JoystickSetSensorsEnabled,
    GAMEINPUT_JoystickUpdate,
    GAMEINPUT_JoystickClose,
    GAMEINPUT_JoystickQuit,
    GAMEINPUT_JoystickGetGamepadMapping
};


#endif /* SDL_JOYSTICK_GAMEINPUT */
