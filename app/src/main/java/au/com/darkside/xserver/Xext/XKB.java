package au.com.darkside.xserver.Xext;

import java.io.IOException;

import au.com.darkside.xserver.Client;
import au.com.darkside.xserver.ErrorCode;
import au.com.darkside.xserver.InputOutput;
import au.com.darkside.xserver.Util;
import au.com.darkside.xserver.XServer;

/**
 * XKB (X Keyboard Extension) — version 1.0.
 *
 * Implements UseExtension, SelectEvents, GetMap, and PerClientFlags
 * to satisfy GTK3 and xterm keyboard initialization.
 */
public class XKB {
    public static final byte EventBase = (byte) 85;
    public static final byte ErrorBase = (byte) 137;

    private static final byte OP_USE_EXTENSION = 0;
    private static final byte OP_SELECT_EVENTS = 1;
    private static final byte OP_BELL = 3;
    private static final byte OP_GET_STATE = 4;
    private static final byte OP_GET_MAP = 8;
    private static final byte OP_PER_CLIENT_FLAGS = 21;

    // MapPart bitmask values
    private static final int MAP_KEY_TYPES = 0x01;
    private static final int MAP_KEY_SYMS = 0x02;
    private static final int MAP_MODIFIER_MAP = 0x04;
    private static final int MAP_EXPLICIT = 0x08;
    private static final int MAP_KEY_ACTIONS = 0x10;
    private static final int MAP_KEY_BEHAVIORS = 0x20;
    private static final int MAP_VIRTUAL_MODS = 0x40;
    private static final int MAP_VIRTUAL_MOD_MAP = 0x80;

    /**
     * Process an XKB extension request.
     */
    public static void processRequest(XServer xServer, Client client, byte opcode,
                                       byte arg, int bytesRemaining) throws IOException {
        InputOutput io = client.getInputOutput();

        switch (arg) {
            case OP_USE_EXTENSION:
                processUseExtension(client, io, opcode, bytesRemaining);
                break;
            case OP_SELECT_EVENTS:
                processSelectEvents(client, io, opcode, bytesRemaining);
                break;
            case OP_BELL:
                // Bell request: 2 deviceSpec + 2 bellClass + 2 bellID +
                // 1 percent + 1 forceSound + 1 eventOnly + 1 pad +
                // 2 pitch + 2 duration + 2 pad + 4 name + 4 window = 20 bytes
                // Void request — consume and ignore (no audible bell on Android).
                io.readSkip(bytesRemaining);
                break;
            case OP_GET_STATE:
                processGetState(client, io, opcode, bytesRemaining);
                break;
            case OP_GET_MAP:
                processGetMap(xServer, client, io, opcode, bytesRemaining);
                break;
            case OP_PER_CLIENT_FLAGS:
                processPerClientFlags(client, io, opcode, bytesRemaining);
                break;
            default:
                io.readSkip(bytesRemaining);
                ErrorCode.write(client, ErrorCode.Implementation, opcode, 0);
                break;
        }
    }

    // ---- UseExtension (opcode 0) ----
    // Request: 2 CARD16 wantedMajor, 2 CARD16 wantedMinor
    // Reply: 1 BOOL supported, 2 CARD16 serverMajor, 2 CARD16 serverMinor, 20 pad
    private static void processUseExtension(Client client, InputOutput io,
                                             byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining < 4) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int wantedMajor = io.readShort();
        int wantedMinor = io.readShort();
        bytesRemaining -= 4;
        io.readSkip(bytesRemaining);

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 1); // supported = true
            io.writeInt(0);              // reply length
            io.writeShort((short) 1);    // server major
            io.writeShort((short) 0);    // server minor
            io.writePadBytes(20);
        }
        io.flush();
    }

    // ---- SelectEvents (opcode 1) ----
    // Request: variable length. No reply (void).
    // We accept and consume the request without storing event masks,
    // since we don't generate XKB events.
    private static void processSelectEvents(Client client, InputOutput io,
                                             byte opcode, int bytesRemaining) throws IOException {
        io.readSkip(bytesRemaining);
        // No reply — void request. Events accepted but not delivered.
    }

    // ---- GetMap (opcode 8) ----
    // Request: 24 bytes (deviceSpec, full, partial, ranges...)
    // Reply: 40 bytes fixed header + variable data per present bits
    //
    // We return the keyboard's actual keycode range but with present=0
    // when no components are requested, or with minimal KeyTypes + KeySyms
    // when those are requested.
    private static void processGetMap(XServer xServer, Client client, InputOutput io,
                                       byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining < 24) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }

        int deviceSpec = io.readShort();     // 2
        int full = io.readShort();           // 2 — MapPart bitmask
        int partial = io.readShort();        // 2
        int firstType = io.readByte();       // 1
        int nTypes = io.readByte();          // 1
        int firstKeySym = io.readByte();     // 1
        int nKeySyms = io.readByte();        // 1
        int firstKeyAction = io.readByte();  // 1
        int nKeyActions = io.readByte();     // 1
        int firstKeyBehavior = io.readByte();// 1
        int nKeyBehaviors = io.readByte();   // 1
        int virtualMods = io.readShort();    // 2
        int firstKeyExplicit = io.readByte();// 1
        int nKeyExplicit = io.readByte();    // 1
        int firstModMapKey = io.readByte();  // 1
        int nModMapKeys = io.readByte();     // 1
        int firstVModMapKey = io.readByte(); // 1
        int nVModMapKeys = io.readByte();    // 1
        io.readSkip(2);                      // 2 pad
        bytesRemaining -= 24;
        io.readSkip(bytesRemaining);

        // Combine full and partial — we return what's requested
        int present = full | partial;

        // Keyboard range: keycodes 8-255 (standard X11)
        int minKeyCode = 8;
        int maxKeyCode = 255;
        int numKeys = maxKeyCode - minKeyCode + 1;

        // Build variable data based on present bits
        // For a minimal server we provide:
        // - KeyTypes: 1 type (ONE_LEVEL) with 1 group, no map entries
        // - KeySyms: empty (nKeySyms=0, totalSyms=0)
        // - Everything else: empty counts

        int replyNTypes = 0;
        int replyTotalTypes = 1; // server always has at least ONE_LEVEL
        int replyFirstType = 0;
        int replyNKeySyms = 0;
        int replyTotalSyms = 0;
        int replyFirstKeySym = (byte) minKeyCode;

        // Calculate variable data size
        int varBytes = 0;

        if ((present & MAP_KEY_TYPES) != 0) {
            // One KeyType struct: 1 byte mods_mask, 1 byte mods_mods,
            // 2 bytes mods_vmods, 1 byte numLevels, 1 byte nMapEntries,
            // 1 byte hasPreserve, 1 byte pad = 8 bytes, 0 map entries
            replyNTypes = 1;
            replyFirstType = 0;
            varBytes += 8; // one minimal KeyType
        }

        if ((present & MAP_KEY_SYMS) != 0) {
            // We return 0 KeySymMap structs — no syms
            replyNKeySyms = 0;
            replyTotalSyms = 0;
        }

        // All other components: return 0 counts, no variable data

        int replyLenWords = (varBytes + 8 + 3) / 4;
        // reply length = (total_reply_size - 32) / 4
        // total = 40 + varBytes, so replyLen = (40 + varBytes - 32) / 4 = (8 + varBytes) / 4
        replyLenWords = (8 + varBytes + 3) / 4;

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0); // deviceID = 0
            io.writeInt(replyLenWords);               // reply length

            // Fixed reply body (32 bytes after the 8-byte header)
            io.writePadBytes(2);                       // pad
            io.writeByte((byte) minKeyCode);           // minKeyCode
            io.writeByte((byte) maxKeyCode);           // maxKeyCode
            io.writeShort((short) present);            // present
            io.writeByte((byte) replyFirstType);       // firstType
            io.writeByte((byte) replyNTypes);          // nTypes
            io.writeByte((byte) replyTotalTypes);      // totalTypes
            io.writeByte((byte) replyFirstKeySym);     // firstKeySym
            io.writeShort((short) replyTotalSyms);     // totalSyms
            io.writeByte((byte) replyNKeySyms);        // nKeySyms
            io.writeByte((byte) 0);                    // firstKeyAction
            io.writeShort((short) 0);                  // totalActions
            io.writeByte((byte) 0);                    // nKeyActions
            io.writeByte((byte) 0);                    // firstKeyBehavior
            io.writeByte((byte) 0);                    // nKeyBehaviors
            io.writeByte((byte) 0);                    // totalKeyBehaviors
            io.writeByte((byte) 0);                    // firstKeyExplicit
            io.writeByte((byte) 0);                    // nKeyExplicit
            io.writeByte((byte) 0);                    // totalKeyExplicit
            io.writeByte((byte) 0);                    // firstModMapKey
            io.writeByte((byte) 0);                    // nModMapKeys
            io.writeByte((byte) 0);                    // totalModMapKeys
            io.writeByte((byte) 0);                    // firstVModMapKey
            io.writeByte((byte) 0);                    // nVModMapKeys
            io.writeByte((byte) 0);                    // totalVModMapKeys
            io.writeByte((byte) 0);                    // pad
            io.writeShort((short) 0);                  // virtualMods

            // Variable data
            if ((present & MAP_KEY_TYPES) != 0) {
                // ONE_LEVEL KeyType:
                io.writeByte((byte) 0);    // mods_mask
                io.writeByte((byte) 0);    // mods_mods
                io.writeShort((short) 0);  // mods_vmods
                io.writeByte((byte) 1);    // numLevels
                io.writeByte((byte) 0);    // nMapEntries
                io.writeByte((byte) 0);    // hasPreserve
                io.writeByte((byte) 0);    // pad
                // 0 map entries, 0 preserve entries
            }
        }
        io.flush();
    }

    // ---- PerClientFlags (opcode 21) ----
    // Request: 2 deviceSpec, 2 pad, 4 change, 4 value, 4 ctrlsToChange,
    //          4 autoCtrls, 4 autoCtrlsValues = 24 bytes
    // Reply: 1 deviceID, 4 supported, 4 value, 4 autoCtrls,
    //        4 autoCtrlsValues, 8 pad = 32 bytes
    private static void processPerClientFlags(Client client, InputOutput io,
                                               byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining < 24) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int deviceSpec = io.readShort();          // 2
        io.readSkip(2);                            // 2 pad
        int change = io.readInt();                 // 4
        int value = io.readInt();                  // 4
        int ctrlsToChange = io.readInt();          // 4
        int autoCtrls = io.readInt();              // 4
        int autoCtrlsValues = io.readInt();        // 4
        bytesRemaining -= 24;
        io.readSkip(bytesRemaining);

        // We support DetectableAutoRepeat (0x01) and report it
        int supported = 0x01; // DetectableAutoRepeat
        int currentValue = value & supported; // accept what we support

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0); // deviceID = 0
            io.writeInt(0);                // reply length
            io.writeInt(supported);        // supported flags
            io.writeInt(currentValue);     // current value
            io.writeInt(0);                // autoCtrls
            io.writeInt(0);                // autoCtrlsValues
            io.writePadBytes(8);
        }
        io.flush();
    }

    // ---- GetState (opcode 4) ----
    // Request: 2 DeviceSpec, 2 pad
    // Reply: all modifier/group state fields, all zero (no modifiers active)
    private static void processGetState(Client client, InputOutput io,
                                         byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining < 4) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int deviceSpec = io.readShort();
        io.readSkip(2); // pad
        bytesRemaining -= 4;
        io.readSkip(bytesRemaining);

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0); // deviceID = 0
            io.writeInt(0);              // reply length
            io.writeByte((byte) 0);      // mods
            io.writeByte((byte) 0);      // baseMods
            io.writeByte((byte) 0);      // latchedMods
            io.writeByte((byte) 0);      // lockedMods
            io.writeByte((byte) 0);      // group
            io.writeByte((byte) 0);      // lockedGroup
            io.writeShort((short) 0);    // baseGroup
            io.writeShort((short) 0);    // latchedGroup
            io.writeByte((byte) 0);      // compatState
            io.writeByte((byte) 0);      // grabMods
            io.writeByte((byte) 0);      // compatGrabMods
            io.writeByte((byte) 0);      // lookupMods
            io.writeByte((byte) 0);      // compatLookupMods
            io.writeByte((byte) 0);      // pad
            io.writeShort((short) 0);    // ptrBtnState
            io.writePadBytes(6);
        }
        io.flush();
    }
}
