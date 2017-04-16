// JOYSTICK API DOCUMENTATION: https://www.kernel.org/doc/Documentation/input/joystick-api.txt

#ifdef __linux__

#include "LinuxJoystickHandler.h"
#include "Utilities/Thread.h"
#include "Utilities/Log.h"

#include <linux/joystick.h>
#include <sys/select.h>
#include <functional>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <cstdio>

namespace {
    const u32 THREAD_SLEEP = 10;
    const u32 THREAD_SLEEP_INACTIVE = 100;
    const u32 READ_TIMEOUT = 10;
    const u32 THREAD_TIMEOUT = 1000;

    // From XInputPadHandler.cpp
    inline u16 ConvertAxis(short value)
    {
        return static_cast<u16>((value + 32768l) >> 8);
    }

    inline u32 milli2micro(u32 milli) { return milli * 1000; }
    inline u32 milli2nano(u32 milli) { return milli * 1000000; }
}

LinuxJoystickConfig g_linux_joystick_config;

LinuxJoystickHandler::LinuxJoystickHandler() {}

LinuxJoystickHandler::~LinuxJoystickHandler() { Close(); }

void LinuxJoystickHandler::Init(const u32 max_connect) {
    std::memset(&m_info, 0, sizeof m_info);
    m_info.max_connect = std::min(max_connect, static_cast<u32>(1));

    for (u32 i = 0; i < m_info.max_connect; ++i) {
        joy_fds.push_back(-1);
        joy_paths.emplace_back(fmt::format("/dev/input/js%d", i));
        m_pads.emplace_back(
            CELL_PAD_STATUS_DISCONNECTED,
            CELL_PAD_SETTING_PRESS_OFF | CELL_PAD_SETTING_SENSOR_OFF,
            CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_PRESS_MODE,
            CELL_PAD_DEV_TYPE_STANDARD
        );
        auto& pad = m_pads.back();

        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, g_linux_joystick_config.triangle, CELL_PAD_CTRL_TRIANGLE);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, g_linux_joystick_config.circle, CELL_PAD_CTRL_CIRCLE);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, g_linux_joystick_config.cross, CELL_PAD_CTRL_CROSS);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, g_linux_joystick_config.square, CELL_PAD_CTRL_SQUARE);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, g_linux_joystick_config.l2, CELL_PAD_CTRL_L2);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, g_linux_joystick_config.r2, CELL_PAD_CTRL_R2);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, g_linux_joystick_config.l1, CELL_PAD_CTRL_L1);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, g_linux_joystick_config.r1, CELL_PAD_CTRL_R1);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, g_linux_joystick_config.start, CELL_PAD_CTRL_START);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, g_linux_joystick_config.select, CELL_PAD_CTRL_SELECT);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, g_linux_joystick_config.l3, CELL_PAD_CTRL_L3);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, g_linux_joystick_config.r3, CELL_PAD_CTRL_R3);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, 0, 0x100/*CELL_PAD_CTRL_PS*/);// TODO: PS button support

        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, g_linux_joystick_config.up, CELL_PAD_CTRL_UP);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, g_linux_joystick_config.down, CELL_PAD_CTRL_DOWN);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, g_linux_joystick_config.left, CELL_PAD_CTRL_LEFT);
        pad.m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, g_linux_joystick_config.right, CELL_PAD_CTRL_RIGHT);

        pad.m_sticks.emplace_back(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X, 0, 0);
        pad.m_sticks.emplace_back(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y, 0, 0);
        pad.m_sticks.emplace_back(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X, 0, 0);
        pad.m_sticks.emplace_back(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y, 0, 0);
    }

    update_fds();
    thread_ctrl::spawn(std::string{thread_name},
                       std::bind(&LinuxJoystickHandler::thread_func, this));
}

void LinuxJoystickHandler::update_fds() {
    int connected=0;

    for (u32 i = 0; i < m_info.max_connect; ++i)
        if (try_open_fd(joy_fds.size()-1)) ++connected;

    m_info.now_connect = connected;
}

bool LinuxJoystickHandler::try_open_fd(u32 index) {
    auto& fd = joy_fds[index];
    bool was_connected = fd != -1;

    const auto& path = joy_paths[index];

    if (!fs::exists(path)) {
        if (was_connected)
            // It was disconnected.
            m_pads[index].m_port_status |= CELL_PAD_STATUS_ASSIGN_CHANGES;
        m_pads[index].m_port_status &= ~CELL_PAD_STATUS_CONNECTED;
        LOG_ERROR(GENERAL, "Joystick %s is not present [previous status: %d]", path.c_str(),
                  was_connected ? 1 : 0);
        fd = -1;
        return false;
    }

    if (was_connected) return true;  // It's already been connected, and the js is still present.
    fd = open(path.c_str(), O_RDONLY);

    if (fd == -1) {
        int err = errno;
        LOG_ERROR(GENERAL, "Failed to open joystick: %s [errno %d]", strerror(err), err);
        return false;
    }

    LOG_NOTICE(GENERAL, "Opened joystick at %s (fd %d)", path, fd);

    if (!was_connected)
        // Connection status changed from disconnected to connected.
        m_pads[index].m_port_status |= CELL_PAD_STATUS_ASSIGN_CHANGES;
    m_pads[index].m_port_status |= CELL_PAD_STATUS_CONNECTED;
    return true;
}

void LinuxJoystickHandler::Close() {
    if (active.load()) {
        active.store(false);
        if (!dead.load())
            if (!thread_ctrl::wait_for(milli2nano(THREAD_TIMEOUT)))
                LOG_ERROR(GENERAL, "LinuxJoystick thread could not stop within %d milliseconds", THREAD_TIMEOUT);
        dead.store(false);
    }
}

void LinuxJoystickHandler::thread_func() {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = milli2nano(READ_TIMEOUT);

    while (active) {
        update_fds();

        fd_set fds_to_wait;
        int max=0;
        FD_ZERO(&fds_to_wait);

        for (int fd : joy_fds) {
            if (fd == -1) continue;
            FD_SET(fd, &fds_to_wait);
            max = std::max(max, fd);
        }

        // Query which joysticks are ready for reading.
        pselect(max+1, &fds_to_wait, NULL, NULL, &ts, NULL);

        for (int i=0; i<joy_fds.size(); i++) {
            auto& pad = m_pads[i];
            int fd = joy_fds[i];
            auto& path = joy_paths[i];

            // Skip if not connected or not ready for reading.
            if (fd == -1 || !FD_ISSET(fd, &fds_to_wait)) continue;

            // Joystick is open: try reading a message from it.
            struct js_event evt;
            if (read(fd, &evt, sizeof(evt)) == -1) {
                int err = errno;
                // NOTE: ENODEV == joystick unplugged
                LOG_ERROR(GENERAL, "Failed to read joystick %s: %s [errno %d]",
                          path.c_str(), strerror(err), err);
                continue;
            }

            // Event message was succesfully read. Mask out JS_EVENT_INIT, because it doesn't really matter.
            evt.type &= ~JS_EVENT_INIT;

            switch (evt.type) {
            case JS_EVENT_BUTTON: {
                auto which_button = std::find_if(
                    pad.m_buttons.begin(), pad.m_buttons.end(),
                    [&](const Button& bt) { return bt.m_keyCode == evt.number; });
                if (which_button == pad.m_buttons.end()) {
                    LOG_ERROR(GENERAL, "Joystick %s sent button event for invalid button %d",
                              path.c_str(), evt.number);
                    break;
                }

                which_button->m_pressed = evt.value;
                which_button->m_value = evt.value ? 255 : 0;
                break;
            }
            case JS_EVENT_AXIS:
                // Joystick event axis #'s should correspond with rpcs3's.
                if (evt.number > pad.m_sticks.size()) {
                    LOG_ERROR(GENERAL, "Joystick %s sent axis event for invalid axis %d",
                              path.c_str(), evt.number);
                }

                pad.m_sticks[evt.number].m_value = ConvertAxis(evt.value);
                break;
            default:
                LOG_ERROR(GENERAL, "Unknown joystick %s event %d", path.c_str(), evt.type);
                break;
            }
        }

        int to_sleep = m_info.now_connect > 0 ? THREAD_SLEEP : THREAD_SLEEP_INACTIVE;
        usleep(milli2micro(to_sleep));
    }

    dead = true;
}

#endif
