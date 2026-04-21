package au.com.darkside.xserver.Xext;

import java.io.IOException;

import android.graphics.Rect;

import au.com.darkside.xserver.Client;
import au.com.darkside.xserver.ErrorCode;
import au.com.darkside.xserver.InputOutput;
import au.com.darkside.xserver.ScreenView;
import au.com.darkside.xserver.Util;
import au.com.darkside.xserver.Window;
import au.com.darkside.xserver.XServer;

/**
 * XINERAMA extension (version 1.1).
 *
 * Reports screen geometry. Single screen — one monitor covering
 * the full root window dimensions.
 */
public class Xinerama {
    private static final byte OP_QUERY_VERSION = 0;
    private static final byte OP_GET_STATE = 1;
    private static final byte OP_GET_SCREEN_COUNT = 2;
    private static final byte OP_GET_SCREEN_SIZE = 3;
    private static final byte OP_IS_ACTIVE = 4;
    private static final byte OP_QUERY_SCREENS = 5;

    /**
     * Process a Xinerama extension request.
     */
    public static void processRequest(XServer xServer, Client client, byte opcode,
                                       byte arg, int bytesRemaining) throws IOException {
        InputOutput io = client.getInputOutput();

        switch (arg) {
            case OP_QUERY_VERSION:
                processQueryVersion(client, io, opcode, bytesRemaining);
                break;
            case OP_GET_STATE:
                processGetState(client, io, opcode, bytesRemaining);
                break;
            case OP_GET_SCREEN_COUNT:
                processGetScreenCount(client, io, opcode, bytesRemaining);
                break;
            case OP_GET_SCREEN_SIZE:
                processGetScreenSize(xServer, client, io, opcode, bytesRemaining);
                break;
            case OP_IS_ACTIVE:
                processIsActive(client, io, opcode, bytesRemaining);
                break;
            case OP_QUERY_SCREENS:
                processQueryScreens(xServer, client, io, opcode, bytesRemaining);
                break;
            default:
                io.readSkip(bytesRemaining);
                ErrorCode.write(client, ErrorCode.Implementation, opcode, 0);
                break;
        }
    }

    // ---- QueryVersion (opcode 0) ----
    // Request: 1 CARD8 major, 1 CARD8 minor, 2 pad
    // Reply: 2 CARD16 major, 2 CARD16 minor, 20 pad
    private static void processQueryVersion(Client client, InputOutput io,
                                             byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining < 4) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        io.readByte();  // client major
        io.readByte();  // client minor
        io.readSkip(2); // pad
        bytesRemaining -= 4;
        io.readSkip(bytesRemaining);

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(0);              // reply length
            io.writeShort((short) 1);    // major version
            io.writeShort((short) 1);    // minor version
            io.writePadBytes(20);
        }
        io.flush();
    }

    // ---- GetState (opcode 1) ----
    // Request: 4 WINDOW
    // Reply: 1 BYTE state, 4 WINDOW, 20 pad
    private static void processGetState(Client client, InputOutput io,
                                         byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining != 4) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int window = io.readInt();

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 1); // state=1 (active)
            io.writeInt(0);           // reply length
            io.writeInt(window);      // window
            io.writePadBytes(20);
        }
        io.flush();
    }

    // ---- GetScreenCount (opcode 2) ----
    // Request: 4 WINDOW
    // Reply: 1 BYTE screen_count, 4 WINDOW, 20 pad
    private static void processGetScreenCount(Client client, InputOutput io,
                                               byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining != 4) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int window = io.readInt();

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 1); // screen_count=1
            io.writeInt(0);           // reply length
            io.writeInt(window);      // window
            io.writePadBytes(20);
        }
        io.flush();
    }

    // ---- GetScreenSize (opcode 3) ----
    // Request: 4 WINDOW, 4 CARD32 screen
    // Reply: 4 CARD32 width, 4 CARD32 height, 4 WINDOW, 4 CARD32 screen, 8 pad
    private static void processGetScreenSize(XServer xServer, Client client, InputOutput io,
                                              byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining != 8) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int window = io.readInt();
        int screen = io.readInt();

        Rect rootRect = getRootRect(xServer);

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(0);                     // reply length
            io.writeInt(rootRect.width());      // width
            io.writeInt(rootRect.height());     // height
            io.writeInt(window);                // window
            io.writeInt(screen);                // screen
            io.writePadBytes(8);
        }
        io.flush();
    }

    // ---- IsActive (opcode 4) ----
    // Request: empty
    // Reply: 4 CARD32 state, 20 pad
    private static void processIsActive(Client client, InputOutput io,
                                         byte opcode, int bytesRemaining) throws IOException {
        io.readSkip(bytesRemaining);

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(0);           // reply length
            io.writeInt(1);           // state=1 (active)
            io.writePadBytes(20);
        }
        io.flush();
    }

    // ---- QueryScreens (opcode 5) ----
    // Request: empty
    // Reply: 4 CARD32 number, 20 pad, then number * ScreenInfo(8 bytes each)
    //   ScreenInfo: 2 INT16 x, 2 INT16 y, 2 CARD16 width, 2 CARD16 height
    private static void processQueryScreens(XServer xServer, Client client, InputOutput io,
                                              byte opcode, int bytesRemaining) throws IOException {
        io.readSkip(bytesRemaining);

        Rect rootRect = getRootRect(xServer);
        int numScreens = 1;
        // reply length = (numScreens * 8) / 4 = 2 words per screen
        int replyLen = numScreens * 2;

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(replyLen);                    // reply length
            io.writeInt(numScreens);                  // number of screens
            io.writePadBytes(20);
            // ScreenInfo for screen 0
            io.writeShort((short) 0);                 // x_org
            io.writeShort((short) 0);                 // y_org
            io.writeShort((short) rootRect.width());  // width
            io.writeShort((short) rootRect.height()); // height
        }
        io.flush();
    }

    /**
     * Get the root window rectangle from the X server.
     */
    private static Rect getRootRect(XServer xServer) {
        ScreenView screen = xServer.getScreen();
        if (screen != null) {
            Window root = screen.getRootWindow();
            if (root != null) {
                return root.getIRect();
            }
        }
        // Fallback if root window not yet created
        return new Rect(0, 0, 1920, 1080);
    }
}
