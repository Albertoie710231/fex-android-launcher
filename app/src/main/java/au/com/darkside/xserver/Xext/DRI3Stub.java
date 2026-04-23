package au.com.darkside.xserver.Xext;

import java.io.IOException;

import au.com.darkside.xserver.Client;
import au.com.darkside.xserver.ErrorCode;
import au.com.darkside.xserver.InputOutput;
import au.com.darkside.xserver.Util;
import au.com.darkside.xserver.XServer;

/**
 * Minimal DRI3 extension stub.
 *
 * Wine's winex11.drv calls `XQueryExtension("DRI3")` at startup before
 * attempting VK_KHR_xlib_surface creation. If DRI3 is absent, wine aborts
 * with "No DRI3 support detected - required for presentation" and refuses
 * to create any Vulkan surface on the X server.
 *
 * This stub advertises DRI3 as present and answers QueryVersion. It does
 * NOT actually implement DMA-BUF fd passing (PIXMAP_FROM_BUFFER*) — those
 * would require ancillary fd reading from the unix socket, a SysV SHM
 * mapper, and a GPU image backend, none of which Darkside has.
 *
 * Expected outcome: wine treats the X server as DRI3-capable and proceeds
 * to create a VkSurfaceKHR. DXVK's presentation path (vkQueuePresentKHR)
 * then either:
 *   (a) succeeds via Mali driver's own xlib-surface → display backend, OR
 *   (b) fails later when libxcb-dri3 actually calls DRI3PixmapFromBuffer
 *       on our stub — at which point we know stubs are insufficient and
 *       we need the full Winlator XConnector swap.
 *
 * Opcodes match Winlator's XConnector (DRI3Extension.java in upstream) so
 * that client-side libxcb-dri3 sends the same wire format.
 */
public class DRI3Stub {
    private static final byte OP_QUERY_VERSION     = 0;
    private static final byte OP_OPEN              = 1;
    private static final byte OP_PIXMAP_FROM_BUFFER = 2;
    private static final byte OP_PIXMAP_FROM_BUFFERS = 7;

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
                io.readInt();  // major version requested
                io.readInt();  // minor version requested
                synchronized (io) {
                    Util.writeReplyHeader(client, (byte) 0);
                    io.writeInt(0);            // Reply length (32-byte total, no extra data).
                    io.writeInt(1);            // Server major version (1).
                    io.writeInt(0);            // Server minor version (0).
                    io.writePadBytes(16);
                }
                io.flush();
                break;

            case OP_OPEN:
                // DRI3 Open normally returns a DRM device fd as ancillary data;
                // the reply header's 2nd byte is `nfd` (number of attached fds).
                // Darkside doesn't support ancillary fd sending, so we reply
                // with nfd=0. Clients that fall back from "no fd" to software/
                // non-DMA-BUF paths will proceed; clients that HARD-require an
                // fd will fail here. Return an error instead of silently lying.
                if (bytesRemaining < 8) {
                    io.readSkip(bytesRemaining);
                    ErrorCode.write(client, ErrorCode.Length, majorOpcode, 0);
                    return;
                }
                io.readInt();  // drawable
                io.readInt();  // provider
                io.readSkip(bytesRemaining - 8);
                // Report BadMatch — "No DRM device available". libxcb-dri3
                // treats this as DRI3Open failure and wine/DXVK then may fall
                // back to a different presentation path (possibly still via
                // our headless layer if it's enabled).
                ErrorCode.write(client, ErrorCode.Match, majorOpcode, 0);
                break;

            case OP_PIXMAP_FROM_BUFFER:
            case OP_PIXMAP_FROM_BUFFERS:
                // These carry ancillary file descriptors (SCM_RIGHTS) with
                // the pixmap buffer the client wants to import. Darkside's
                // InputOutput doesn't read ancillary data, so we can't pick
                // up the fd — we just skip the request payload. Wine/DXVK
                // will see the pixmap as never-imported; if their code path
                // depends on the pixmap being drawable, this is where the
                // stub becomes insufficient. Error responses would still be
                // a BadAlloc; for now just drain and do nothing (no reply
                // expected — these requests are one-way in X protocol).
                io.readSkip(bytesRemaining);
                break;

            default:
                io.readSkip(bytesRemaining);
                ErrorCode.write(client, ErrorCode.Implementation, majorOpcode, 0);
                break;
        }
    }
}
