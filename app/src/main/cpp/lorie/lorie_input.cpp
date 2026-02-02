/**
 * Lorie Input Handler
 *
 * Translates Android input events to X11 input events.
 * Handles touch, keyboard, and gamepad input.
 */

#include <android/log.h>
#include <android/keycodes.h>
#include <map>

#define LOG_TAG "LorieInput"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace lorie {

// X11 keysym definitions
namespace xk {
    constexpr int Escape = 0xff1b;
    constexpr int Tab = 0xff09;
    constexpr int Return = 0xff0d;
    constexpr int BackSpace = 0xff08;
    constexpr int Delete = 0xffff;
    constexpr int Home = 0xff50;
    constexpr int End = 0xff57;
    constexpr int Page_Up = 0xff55;
    constexpr int Page_Down = 0xff56;
    constexpr int Left = 0xff51;
    constexpr int Up = 0xff52;
    constexpr int Right = 0xff53;
    constexpr int Down = 0xff54;
    constexpr int Shift_L = 0xffe1;
    constexpr int Shift_R = 0xffe2;
    constexpr int Control_L = 0xffe3;
    constexpr int Control_R = 0xffe4;
    constexpr int Alt_L = 0xffe9;
    constexpr int Alt_R = 0xffea;
    constexpr int Super_L = 0xffeb;
    constexpr int Super_R = 0xffec;
    constexpr int F1 = 0xffbe;
    constexpr int space = 0x0020;
}

class InputHandler {
public:
    InputHandler() = default;

    /**
     * Convert Android keycode to X11 keysym.
     */
    int androidToX11KeyCode(int androidKeyCode) {
        switch (androidKeyCode) {
            // Letters
            case AKEYCODE_A: return 'a';
            case AKEYCODE_B: return 'b';
            case AKEYCODE_C: return 'c';
            case AKEYCODE_D: return 'd';
            case AKEYCODE_E: return 'e';
            case AKEYCODE_F: return 'f';
            case AKEYCODE_G: return 'g';
            case AKEYCODE_H: return 'h';
            case AKEYCODE_I: return 'i';
            case AKEYCODE_J: return 'j';
            case AKEYCODE_K: return 'k';
            case AKEYCODE_L: return 'l';
            case AKEYCODE_M: return 'm';
            case AKEYCODE_N: return 'n';
            case AKEYCODE_O: return 'o';
            case AKEYCODE_P: return 'p';
            case AKEYCODE_Q: return 'q';
            case AKEYCODE_R: return 'r';
            case AKEYCODE_S: return 's';
            case AKEYCODE_T: return 't';
            case AKEYCODE_U: return 'u';
            case AKEYCODE_V: return 'v';
            case AKEYCODE_W: return 'w';
            case AKEYCODE_X: return 'x';
            case AKEYCODE_Y: return 'y';
            case AKEYCODE_Z: return 'z';

            // Numbers
            case AKEYCODE_0: return '0';
            case AKEYCODE_1: return '1';
            case AKEYCODE_2: return '2';
            case AKEYCODE_3: return '3';
            case AKEYCODE_4: return '4';
            case AKEYCODE_5: return '5';
            case AKEYCODE_6: return '6';
            case AKEYCODE_7: return '7';
            case AKEYCODE_8: return '8';
            case AKEYCODE_9: return '9';

            // Special keys
            case AKEYCODE_SPACE: return xk::space;
            case AKEYCODE_ENTER: return xk::Return;
            case AKEYCODE_TAB: return xk::Tab;
            case AKEYCODE_ESCAPE: return xk::Escape;
            case AKEYCODE_DEL: return xk::BackSpace;
            case AKEYCODE_FORWARD_DEL: return xk::Delete;

            // Navigation
            case AKEYCODE_DPAD_UP: return xk::Up;
            case AKEYCODE_DPAD_DOWN: return xk::Down;
            case AKEYCODE_DPAD_LEFT: return xk::Left;
            case AKEYCODE_DPAD_RIGHT: return xk::Right;
            case AKEYCODE_MOVE_HOME: return xk::Home;
            case AKEYCODE_MOVE_END: return xk::End;
            case AKEYCODE_PAGE_UP: return xk::Page_Up;
            case AKEYCODE_PAGE_DOWN: return xk::Page_Down;

            // Modifiers
            case AKEYCODE_SHIFT_LEFT: return xk::Shift_L;
            case AKEYCODE_SHIFT_RIGHT: return xk::Shift_R;
            case AKEYCODE_CTRL_LEFT: return xk::Control_L;
            case AKEYCODE_CTRL_RIGHT: return xk::Control_R;
            case AKEYCODE_ALT_LEFT: return xk::Alt_L;
            case AKEYCODE_ALT_RIGHT: return xk::Alt_R;
            case AKEYCODE_META_LEFT: return xk::Super_L;
            case AKEYCODE_META_RIGHT: return xk::Super_R;

            // Function keys
            case AKEYCODE_F1: return xk::F1;
            case AKEYCODE_F2: return xk::F1 + 1;
            case AKEYCODE_F3: return xk::F1 + 2;
            case AKEYCODE_F4: return xk::F1 + 3;
            case AKEYCODE_F5: return xk::F1 + 4;
            case AKEYCODE_F6: return xk::F1 + 5;
            case AKEYCODE_F7: return xk::F1 + 6;
            case AKEYCODE_F8: return xk::F1 + 7;
            case AKEYCODE_F9: return xk::F1 + 8;
            case AKEYCODE_F10: return xk::F1 + 9;
            case AKEYCODE_F11: return xk::F1 + 10;
            case AKEYCODE_F12: return xk::F1 + 11;

            // Symbols
            case AKEYCODE_MINUS: return '-';
            case AKEYCODE_EQUALS: return '=';
            case AKEYCODE_LEFT_BRACKET: return '[';
            case AKEYCODE_RIGHT_BRACKET: return ']';
            case AKEYCODE_BACKSLASH: return '\\';
            case AKEYCODE_SEMICOLON: return ';';
            case AKEYCODE_APOSTROPHE: return '\'';
            case AKEYCODE_COMMA: return ',';
            case AKEYCODE_PERIOD: return '.';
            case AKEYCODE_SLASH: return '/';
            case AKEYCODE_GRAVE: return '`';

            default:
                LOGD("Unknown Android keycode: %d", androidKeyCode);
                return 0;
        }
    }

    /**
     * Convert Android touch action to X11 button event.
     */
    struct MouseEvent {
        int button;  // 1=left, 2=middle, 3=right, 4=scroll up, 5=scroll down
        bool press;
        float x, y;
    };

    MouseEvent touchToMouse(int action, float x, float y) {
        MouseEvent event = {0, false, x, y};

        switch (action) {
            case 0: // ACTION_DOWN
                event.button = 1;
                event.press = true;
                break;
            case 1: // ACTION_UP
                event.button = 1;
                event.press = false;
                break;
            case 2: // ACTION_MOVE
                event.button = 0; // Motion only
                break;
        }

        return event;
    }

    /**
     * Convert gamepad button to keyboard key.
     */
    int gamepadButtonToKey(int button) {
        switch (button) {
            case 96:  // BUTTON_A
                return xk::Return;
            case 97:  // BUTTON_B
                return xk::Escape;
            case 99:  // BUTTON_X
                return xk::space;
            case 100: // BUTTON_Y
                return xk::Tab;
            default:
                return 0;
        }
    }

    /**
     * Get modifier state from Android meta state.
     */
    struct ModifierState {
        bool shift;
        bool ctrl;
        bool alt;
        bool super;
    };

    ModifierState getModifiers(int metaState) {
        ModifierState mods = {false, false, false, false};
        mods.shift = (metaState & 0x1) != 0;  // META_SHIFT_ON
        mods.ctrl = (metaState & 0x1000) != 0; // META_CTRL_ON
        mods.alt = (metaState & 0x2) != 0;     // META_ALT_ON
        mods.super = (metaState & 0x10000) != 0; // META_META_ON
        return mods;
    }
};

} // namespace lorie
