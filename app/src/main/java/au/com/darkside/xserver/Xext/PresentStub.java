package au.com.darkside.xserver.Xext;

import java.io.IOException;

import au.com.darkside.xserver.Client;
import au.com.darkside.xserver.ErrorCode;
import au.com.darkside.xserver.InputOutput;
import au.com.darkside.xserver.Util;
import au.com.darkside.xserver.XServer;

/**
 * Minimal Present extension stub.
 *
 * Paired with DRI3Stub: wine's winex11.drv checks BOTH DRI3 and Present
 * extensions at Vulkan-surface creation time. If either is absent, VK
 * surface creation fails.
 *
 * This stub answers QueryVersion and silently accepts PresentPixmap +
 * SelectInput. It does NOT fire PresentCompleteNotify events back to the
 * client — wine's WSI may block on that if it actually uses the Present
 * path for VSync. If the game hangs waiting for CompleteNotify we'd need
 * to upgrade this to generate synthetic events at 60Hz.
 *
 * Opcodes match Winlator's XConnector (PresentExtension.java) to match
 * libxcb-present's wire format.
 */
public class PresentStub {
    private static final byte OP_QUERY_VERSION  = 0;
    private static final byte OP_PRESENT_PIXMAP = 1;
    private static final byte OP_SELECT_INPUT   = 3;

    public static void processRequest(XServer xServer, Client client, byte majorOpcode,
                                      byte minorOpcode, int bytesRemaining) throws IOException {
        InputOutput io = client.getInputOutput();
        switch (minorOpcode) {
            case OP_QUERY_VERSION:
                if (bytesRemaining != 8) {
                    io.readSkip(bytesRemaining);
                    ErrorCode.write(client, ErrorCode.Length, majorOpcode, 0);
                    return;
                }
                io.readInt();  // requested major
                io.readInt();  // requested minor
                synchronized (io) {
                    Util.writeReplyHeader(client, (byte) 0);
                    io.writeInt(0);
                    io.writeInt(1);
                    io.writeInt(0);
                    io.writePadBytes(16);
                }
                io.flush();
                break;

            case OP_PRESENT_PIXMAP:
                // Fire-and-forget in the X protocol (no reply). Drain and
                // ignore. If wine depends on CompleteNotify we'll see the
                // game hang waiting for PresentCompleteNotify events —
                // signal to upgrade the stub.
                io.readSkip(bytesRemaining);
                break;

            case OP_SELECT_INPUT:
                // Event-mask registration — fire-and-forget in X protocol.
                io.readSkip(bytesRemaining);
                break;

            default:
                io.readSkip(bytesRemaining);
                ErrorCode.write(client, ErrorCode.Implementation, majorOpcode, 0);
                break;
        }
    }
}
