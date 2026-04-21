package au.com.darkside.xserver.Xext;

import java.io.IOException;

import au.com.darkside.xserver.Client;
import au.com.darkside.xserver.InputOutput;
import au.com.darkside.xserver.ScreenView;
import au.com.darkside.xserver.Util;
import au.com.darkside.xserver.XServer;

/**
 * Minimal RANDR extension stub — enough so apps don't crash querying screen info.
 */
public class XRandr {
    public static final byte EventBase = (byte) 89;
    public static final byte ErrorBase = (byte) 147;

    private static final int QueryVersion = 0;
    private static final int SetScreenConfig = 2;
    private static final int SelectInput = 4;
    private static final int GetScreenInfo = 5;
    private static final int GetScreenSizeRange = 6;
    private static final int GetScreenResources = 8;
    private static final int GetOutputInfo = 9;
    private static final int GetCrtcInfo = 20;
    private static final int GetScreenResourcesCurrent = 25;
    private static final int GetProviders = 32;
    private static final int GetProviderInfo = 33;
    private static final int GetMonitors = 42;

    public static void processRequest(XServer xServer, Client client, byte opcode, byte arg, int bytesRemaining) throws IOException {
        InputOutput io = client.getInputOutput();
        int minorOpcode = arg & 0xff;

        switch (minorOpcode) {
            case QueryVersion:
                processQueryVersion(client, io, bytesRemaining);
                break;
            case SelectInput:
                // Just consume — we don't send RANDR events
                io.readSkip(bytesRemaining);
                break;
            case GetScreenResources:
            case GetScreenResourcesCurrent:
                processGetScreenResources(xServer, client, io, bytesRemaining);
                break;
            case GetMonitors:
                processGetMonitors(xServer, client, io, bytesRemaining);
                break;
            case GetScreenSizeRange:
                processGetScreenSizeRange(xServer, client, io, bytesRemaining);
                break;
            case GetOutputInfo:
            case GetCrtcInfo:
            case GetProviders:
            case GetProviderInfo:
            case GetScreenInfo:
                // These need replies — send empty ones
                io.readSkip(bytesRemaining);
                synchronized (io) {
                    Util.writeReplyHeader(client, (byte) 0);
                    io.writeInt(0);
                    io.writePadBytes(24);
                }
                io.flush();
                break;
            default:
                io.readSkip(bytesRemaining);
                // Opcodes that do NOT need a reply (per spec):
                // 4=SelectInput, 7=SetScreenSize, 12=ConfigureOutputProperty,
                // 13=ChangeOutputProperty, 14=DeleteOutputProperty, 17=DestroyMode,
                // 18=AddOutputMode, 19=DeleteOutputMode, 24=SetCrtcGamma,
                // 26=SetCrtcTransform, 30=SetOutputPrimary, 34=SetProviderOffloadSink,
                // 35=SetProviderOutputSource, 38=ConfigureProviderProperty,
                // 39=ChangeProviderProperty, 40=DeleteProviderProperty,
                // 43=SetMonitor, 44=DeleteMonitor, 46=FreeLease
                boolean noReply = (minorOpcode == 4 || minorOpcode == 7 ||
                    (minorOpcode >= 12 && minorOpcode <= 14) || minorOpcode == 17 ||
                    minorOpcode == 18 || minorOpcode == 19 || minorOpcode == 24 ||
                    minorOpcode == 26 || minorOpcode == 30 ||
                    (minorOpcode >= 34 && minorOpcode <= 35) ||
                    (minorOpcode >= 38 && minorOpcode <= 40) ||
                    minorOpcode == 43 || minorOpcode == 44 || minorOpcode == 46);
                if (!noReply) {
                    synchronized (io) {
                        Util.writeReplyHeader(client, (byte) 0);
                        io.writeInt(0);
                        io.writePadBytes(24);
                    }
                    io.flush();
                }
                break;
        }
    }

    private static void processQueryVersion(Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining >= 8) {
            io.readInt(); // client major
            io.readInt(); // client minor
            bytesRemaining -= 8;
        }
        io.readSkip(bytesRemaining);

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(0);
            io.writeInt(1);  // major version 1
            io.writeInt(5);  // minor version 5
            io.writePadBytes(16);
        }
        io.flush();
    }

    private static void processGetScreenResources(XServer xServer, Client client, InputOutput io, int bytesRemaining) throws IOException {
        io.readSkip(bytesRemaining);

        // Return empty screen resources (no outputs, no CRTCs, no modes)
        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(0);  // reply length
            io.writeInt(xServer.getTimestamp()); // timestamp
            io.writeInt(xServer.getTimestamp()); // config timestamp
            io.writeShort((short) 0); // num CRTCs
            io.writeShort((short) 0); // num outputs
            io.writeShort((short) 0); // num modes
            io.writeShort((short) 0); // names length
            io.writePadBytes(8);
        }
        io.flush();
    }

    private static void processGetMonitors(XServer xServer, Client client, InputOutput io, int bytesRemaining) throws IOException {
        io.readSkip(bytesRemaining);

        // Return 0 monitors
        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(0);  // reply length
            io.writeInt(xServer.getTimestamp()); // timestamp
            io.writeInt(0);  // num monitors
            io.writeInt(0);  // num outputs
            io.writePadBytes(12);
        }
        io.flush();
    }

    private static void processGetScreenSizeRange(XServer xServer, Client client, InputOutput io, int bytesRemaining) throws IOException {
        io.readSkip(bytesRemaining);

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(0);
            io.writeShort((short) 1);    // min width
            io.writeShort((short) 1);    // min height
            io.writeShort((short) 32767); // max width
            io.writeShort((short) 32767); // max height
            io.writePadBytes(16);
        }
        io.flush();
    }
}
