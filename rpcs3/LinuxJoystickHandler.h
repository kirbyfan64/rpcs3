#pragma once

#include "Utilities/types.h"
#include "Utilities/Config.h"
#include "Utilities/File.h"
#include <vector>
#include "Emu/Io/PadHandler.h"
#include <libevdev/libevdev.h>

struct LinuxJoystickConfig final : cfg::node
{
    const std::string cfg_name = fs::get_config_dir() + "/config_linuxjoystick.yml";

    cfg::int32_entry start{ *this, "Start", 9 };
    cfg::int32_entry select{ *this, "Select", 10 };
    cfg::int32_entry square{ *this, "Square", 4 };
    cfg::int32_entry cross{ *this, "Cross", 3 };
    cfg::int32_entry circle{ *this, "Circle", 2 };
    cfg::int32_entry triangle{ *this, "Triangle", 1 };

    cfg::int32_entry r1{ *this, "R1", 8 };
    cfg::int32_entry r2{ *this, "R2", 6 };
    cfg::int32_entry r3{ *this, "R3", 12 };
    cfg::int32_entry l1{ *this, "L1", 7 };
    cfg::int32_entry l2{ *this, "L2", 5 };
    cfg::int32_entry l3{ *this, "L3", 11 };

    cfg::int32_entry up{ *this, "Up", 13 };
    cfg::int32_entry down{ *this, "Down", 14 };
    cfg::int32_entry left{ *this, "Left", 15 };
    cfg::int32_entry right{ *this, "Right", 16 };

    bool load()
    {
        if (fs::file cfg_file{ cfg_name, fs::read })
        {
            return from_string(cfg_file.to_string());
        }

        return false;
    }

    void save()
    {
        fs::file(cfg_name, fs::rewrite).write(to_string());
    }
};


class LinuxJoystickHandler final : public PadHandlerBase
{
public:
    LinuxJoystickHandler();
    ~LinuxJoystickHandler();

    void Init(const u32 max_connect) override;
    void Close();

private:
    void update_devs();
    bool try_open_dev(u32 index);
    void thread_func();

    mutable atomic_t<bool> active{false}, dead{false};
    std::vector<std::string> joy_paths;
    std::vector<libevdev*> joy_devs;
};
