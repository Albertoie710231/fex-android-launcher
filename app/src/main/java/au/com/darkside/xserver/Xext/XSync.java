package au.com.darkside.xserver.Xext;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Hashtable;
import java.util.List;

import au.com.darkside.xserver.Client;
import au.com.darkside.xserver.ErrorCode;
import au.com.darkside.xserver.InputOutput;
import au.com.darkside.xserver.Util;
import au.com.darkside.xserver.XServer;

/**
 * X SYNC extension (spec version 3.1).
 *
 * Provides counters and alarms for synchronized
 * window resize between WM (Openbox) and clients (GTK3).
 */
public class XSync {
    public static final byte EventBase = 95;
    public static final byte ErrorBase = (byte) 154;

    // Error codes (offsets from ErrorBase)
    private static final byte ERROR_COUNTER = 0;
    private static final byte ERROR_ALARM = 1;

    // Minor opcodes
    private static final byte OP_INITIALIZE = 0;
    private static final byte OP_LIST_SYSTEM_COUNTERS = 1;
    private static final byte OP_CREATE_COUNTER = 2;
    private static final byte OP_SET_COUNTER = 3;
    private static final byte OP_CHANGE_COUNTER = 4;
    private static final byte OP_QUERY_COUNTER = 5;
    private static final byte OP_DESTROY_COUNTER = 6;
    private static final byte OP_AWAIT = 7;
    private static final byte OP_CREATE_ALARM = 8;
    private static final byte OP_CHANGE_ALARM = 9;
    private static final byte OP_QUERY_ALARM = 10;
    private static final byte OP_DESTROY_ALARM = 11;
    private static final byte OP_SET_PRIORITY = 12;
    private static final byte OP_GET_PRIORITY = 13;

    // Alarm test types
    private static final int POSITIVE_TRANSITION = 0;
    private static final int NEGATIVE_TRANSITION = 1;
    private static final int POSITIVE_COMPARISON = 2;
    private static final int NEGATIVE_COMPARISON = 3;

    // Alarm states
    private static final int ALARM_ACTIVE = 0;
    private static final int ALARM_INACTIVE = 1;
    private static final int ALARM_DESTROYED = 2;

    // Value mask bits for CreateAlarm/ChangeAlarm
    private static final int CA_COUNTER = 0x01;
    private static final int CA_VALUE_TYPE = 0x02;
    private static final int CA_VALUE = 0x04;
    private static final int CA_TEST_TYPE = 0x08;
    private static final int CA_DELTA = 0x10;
    private static final int CA_EVENTS = 0x20;

    // System counters (read-only, server-managed)
    private static final List<SystemCounter> _systemCounters = new ArrayList<>();

    // Client-created counters: id -> Counter
    private static final Hashtable<Integer, Counter> _counters = new Hashtable<>();

    // Client-created alarms: id -> Alarm
    private static final Hashtable<Integer, Alarm> _alarms = new Hashtable<>();

    // Client priorities: client XID -> priority
    private static final Hashtable<Integer, Integer> _priorities = new Hashtable<>();

    public static void Initialize() {
        _systemCounters.clear();
        _systemCounters.add(new SystemCounter(1, 0, 1, "SERVERTIME"));
    }

    /**
     * Process a SYNC extension request.
     */
    public static void processRequest(XServer xServer, Client client, byte opcode,
                                       byte arg, int bytesRemaining) throws IOException {
        InputOutput io = client.getInputOutput();

        switch (arg) {
            case OP_INITIALIZE:
                processInitialize(xServer, client, io, opcode, bytesRemaining);
                break;
            case OP_LIST_SYSTEM_COUNTERS:
                processListSystemCounters(client, io, opcode, bytesRemaining);
                break;
            case OP_CREATE_COUNTER:
                processCreateCounter(client, io, opcode, bytesRemaining);
                break;
            case OP_SET_COUNTER:
                processSetCounter(client, io, opcode, bytesRemaining);
                break;
            case OP_CHANGE_COUNTER:
                processChangeCounter(client, io, opcode, bytesRemaining);
                break;
            case OP_QUERY_COUNTER:
                processQueryCounter(xServer, client, io, opcode, bytesRemaining);
                break;
            case OP_DESTROY_COUNTER:
                processDestroyCounter(client, io, opcode, bytesRemaining);
                break;
            case OP_AWAIT:
                processAwait(client, io, opcode, bytesRemaining);
                break;
            case OP_CREATE_ALARM:
                processCreateAlarm(xServer, client, io, opcode, bytesRemaining);
                break;
            case OP_CHANGE_ALARM:
                processChangeAlarm(xServer, client, io, opcode, bytesRemaining);
                break;
            case OP_QUERY_ALARM:
                processQueryAlarm(client, io, opcode, bytesRemaining);
                break;
            case OP_DESTROY_ALARM:
                processDestroyAlarm(xServer, client, io, opcode, bytesRemaining);
                break;
            case OP_SET_PRIORITY:
                processSetPriority(client, io, opcode, bytesRemaining);
                break;
            case OP_GET_PRIORITY:
                processGetPriority(client, io, opcode, bytesRemaining);
                break;
            default:
                io.readSkip(bytesRemaining);
                ErrorCode.write(client, ErrorCode.Implementation, opcode, 0);
                break;
        }
    }

    // ---- Initialize (opcode 0) ----
    // Request: 1 CARD8 major, 1 CARD8 minor, 2 unused
    // Reply: 1 CARD8 major, 1 CARD8 minor, 2+20 unused
    private static void processInitialize(XServer xServer, Client client, InputOutput io,
                                           byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining < 4) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int clientMajor = io.readByte();
        int clientMinor = io.readByte();
        io.readSkip(2); // unused padding
        bytesRemaining -= 4;
        io.readSkip(bytesRemaining);

        // Server supports 3.1, negotiate minimum
        int major = Math.min(clientMajor, 3);
        int minor = (major < 3) ? clientMinor : Math.min(clientMinor, 1);

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(0);                     // reply length
            io.writeByte((byte) major);
            io.writeByte((byte) minor);
            io.writePadBytes(22);
        }
        io.flush();
    }

    // ---- ListSystemCounters (opcode 1) ----
    // Request: empty (just header)
    // Reply: 4 counters_len, 20 unused, then list of SYSTEMCOUNTER
    private static void processListSystemCounters(Client client, InputOutput io,
                                                   byte opcode, int bytesRemaining) throws IOException {
        io.readSkip(bytesRemaining);

        // Calculate total bytes for the system counter list
        int listBytes = 0;
        for (SystemCounter sc : _systemCounters) {
            // 4 counter + 4 resHi + 4 resLo + 2 nameLen + name + pad
            int entryLen = 14 + sc.name.length;
            int pad = (4 - ((entryLen) % 4)) % 4;
            listBytes += entryLen + pad;
        }
        int replyLen = (listBytes + 24) / 4; // 24 = counters_len(4) + unused(20)
        // Actually reply length = extra bytes beyond the fixed 32-byte header, in 4-byte units
        // Fixed reply = 32 bytes (8 header + 4 counters_len + 20 unused = 32)
        // Extra = listBytes
        int replyLenWords = (listBytes + 3) / 4; // pad to 4-byte boundary
        // But we also have counters_len(4) + unused(20) = 24 bytes in the fixed part
        // Wait - the reply header is: 1 reply + 1 unused + 2 seq + 4 replyLen = 8 bytes
        // Then: 4 counters_len + 20 unused = 24 bytes (total fixed = 32)
        // replyLen = (24 + listBytes) / 4 ... no.
        // Per X11 spec, reply length = (total reply size - 32) / 4
        // total = 32 + listBytes (padded to 4)
        // replyLen = listBytes_padded / 4
        // But counters_len + unused = 24 is INSIDE the 32-byte fixed header
        // Actually no: the header is 8 bytes, then replyLen counts the remaining words
        // remaining = 4 (counters_len) + 20 (unused) + listBytes = 24 + listBytes
        // replyLen = (24 + listBytes_padded) / 4 ... no that's wrong too
        // Let me re-check: writeReplyHeader writes 8 bytes. Then we writeInt(replyLen) = 4 bytes.
        // The replyLen field tells the client how many additional 4-byte words follow AFTER
        // the initial 32 bytes. The initial 32 bytes = 8 (type+unused+seq+replyLen) + 24 more.
        // So those 24 bytes (counters_len + unused) are part of the fixed 32.
        // replyLen = ceil(listBytes / 4)
        int listPadded = ((listBytes + 3) / 4) * 4;

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(listPadded / 4);        // reply length in 4-byte words
            io.writeInt(_systemCounters.size()); // counters_len
            io.writePadBytes(20);                // unused

            for (SystemCounter sc : _systemCounters) {
                io.writeInt(sc.id);             // COUNTER id
                io.writeInt(sc.resHi);          // INT64 resolution hi
                io.writeInt(sc.resLo);          // INT64 resolution lo
                io.writeShort((short) sc.name.length); // name length
                io.writeBytes(sc.name, 0, sc.name.length);
                int entryLen = 14 + sc.name.length;
                int pad = (4 - (entryLen % 4)) % 4;
                if (pad > 0) io.writePadBytes(pad);
            }
        }
        io.flush();
    }

    // ---- CreateCounter (opcode 2) ----
    // Request: 4 COUNTER id, 8 INT64 initial_value. No reply.
    private static void processCreateCounter(Client client, InputOutput io,
                                              byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining != 12) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int id = io.readInt();
        int hi = io.readInt();
        int lo = io.readInt();
        long value = ((long) hi << 32) | (lo & 0xFFFFFFFFL);

        Counter counter = new Counter(id, value, client);
        _counters.put(id, counter);
    }

    // ---- SetCounter (opcode 3) ----
    // Request: 4 COUNTER id, 8 INT64 value. No reply.
    private static void processSetCounter(Client client, InputOutput io,
                                           byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining != 12) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int id = io.readInt();
        int hi = io.readInt();
        int lo = io.readInt();
        long newValue = ((long) hi << 32) | (lo & 0xFFFFFFFFL);

        Counter counter = _counters.get(id);
        if (counter == null) {
            ErrorCode.writeWithMinorOpcode(client, (byte) (ErrorBase + ERROR_COUNTER),
                    (short) OP_SET_COUNTER, opcode, id);
            return;
        }

        long oldValue = counter.value;
        counter.value = newValue;
        checkAlarms(counter, oldValue);
    }

    // ---- ChangeCounter (opcode 4) ----
    // Request: 4 COUNTER id, 8 INT64 delta. No reply.
    private static void processChangeCounter(Client client, InputOutput io,
                                              byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining != 12) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int id = io.readInt();
        int hi = io.readInt();
        int lo = io.readInt();
        long delta = ((long) hi << 32) | (lo & 0xFFFFFFFFL);

        Counter counter = _counters.get(id);
        if (counter == null) {
            ErrorCode.writeWithMinorOpcode(client, (byte) (ErrorBase + ERROR_COUNTER),
                    (short) OP_CHANGE_COUNTER, opcode, id);
            return;
        }

        long oldValue = counter.value;
        counter.value += delta;
        checkAlarms(counter, oldValue);
    }

    // ---- QueryCounter (opcode 5) ----
    // Request: 4 COUNTER id
    // Reply: 8 INT64 value, 16 unused
    private static void processQueryCounter(XServer xServer, Client client, InputOutput io,
                                             byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining != 4) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int id = io.readInt();

        // Check system counters first
        for (SystemCounter sc : _systemCounters) {
            if (sc.id == id) {
                long value = getSystemCounterValue(sc, xServer);
                synchronized (io) {
                    Util.writeReplyHeader(client, (byte) 0);
                    io.writeInt(0);             // reply length
                    io.writeInt((int) (value >> 32));  // hi
                    io.writeInt((int) value);          // lo
                    io.writePadBytes(16);
                }
                io.flush();
                return;
            }
        }

        Counter counter = _counters.get(id);
        if (counter == null) {
            ErrorCode.writeWithMinorOpcode(client, (byte) (ErrorBase + ERROR_COUNTER),
                    (short) OP_QUERY_COUNTER, opcode, id);
            return;
        }

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(0);                             // reply length
            io.writeInt((int) (counter.value >> 32));    // hi
            io.writeInt((int) counter.value);            // lo
            io.writePadBytes(16);
        }
        io.flush();
    }

    // ---- DestroyCounter (opcode 6) ----
    // Request: 4 COUNTER id. No reply (void).
    private static void processDestroyCounter(Client client, InputOutput io,
                                               byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining != 4) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int id = io.readInt();

        Counter counter = _counters.remove(id);
        if (counter == null) {
            ErrorCode.writeWithMinorOpcode(client, (byte) (ErrorBase + ERROR_COUNTER),
                    (short) OP_DESTROY_COUNTER, opcode, id);
            return;
        }

        // Destroy all alarms that reference this counter
        List<Integer> toRemove = new ArrayList<>();
        for (Alarm alarm : _alarms.values()) {
            if (alarm.counterId == id) {
                alarm.state = ALARM_DESTROYED;
                if (alarm.events) {
                    sendAlarmNotify(alarm, counter.value);
                }
                toRemove.add(alarm.id);
            }
        }
        for (int aid : toRemove) {
            _alarms.remove(aid);
        }
    }

    // ---- Await (opcode 7) ----
    // Request: list of WAITCONDITION (28 bytes each). No reply.
    // We check conditions immediately — if met, return. If not, also return
    // (server time always advances, so conditions on SERVERTIME are effectively met).
    private static void processAwait(Client client, InputOutput io,
                                      byte opcode, int bytesRemaining) throws IOException {
        // Each WAITCONDITION = 28 bytes: TRIGGER(20) + event_threshold(8)
        if (bytesRemaining == 0 || bytesRemaining % 28 != 0) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        // Consume the wait conditions
        io.readSkip(bytesRemaining);
        // Await is a blocking request — we return immediately since our counters
        // are always "current" and the conditions are effectively met.
    }

    // ---- CreateAlarm (opcode 8) ----
    // Request: 4 ALARM id, 4 valueMask, then values per mask bits
    private static void processCreateAlarm(XServer xServer, Client client, InputOutput io,
                                            byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining < 8) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int id = io.readInt();
        int valueMask = io.readInt();
        bytesRemaining -= 8;

        Alarm alarm = new Alarm(id, client);
        // Defaults per spec
        alarm.counterId = 0;       // None
        alarm.valueType = 0;       // Absolute
        alarm.value = 0;
        alarm.testType = POSITIVE_COMPARISON;
        alarm.delta = 1;
        alarm.events = true;
        alarm.state = ALARM_ACTIVE;

        bytesRemaining = readAlarmValues(io, alarm, valueMask, bytesRemaining);
        io.readSkip(bytesRemaining);

        _alarms.put(id, alarm);

        // Check if alarm should fire immediately
        checkSingleAlarm(xServer, alarm);
    }

    // ---- ChangeAlarm (opcode 9) ----
    // Same format as CreateAlarm but for existing alarm
    private static void processChangeAlarm(XServer xServer, Client client, InputOutput io,
                                            byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining < 8) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int id = io.readInt();
        int valueMask = io.readInt();
        bytesRemaining -= 8;

        Alarm alarm = _alarms.get(id);
        if (alarm == null) {
            io.readSkip(bytesRemaining);
            ErrorCode.writeWithMinorOpcode(client, (byte) (ErrorBase + ERROR_ALARM),
                    (short) OP_CHANGE_ALARM, opcode, id);
            return;
        }

        bytesRemaining = readAlarmValues(io, alarm, valueMask, bytesRemaining);
        io.readSkip(bytesRemaining);

        // Reactivate if it was inactive
        if (alarm.state == ALARM_INACTIVE) {
            alarm.state = ALARM_ACTIVE;
        }

        checkSingleAlarm(xServer, alarm);
    }

    // ---- QueryAlarm (opcode 10) ----
    // Request: 4 ALARM id
    // Reply: TRIGGER(20) + INT64 delta(8) + BOOL events(1) + CARD8 state(1) + 2 unused
    private static void processQueryAlarm(Client client, InputOutput io,
                                           byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining != 4) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int id = io.readInt();

        Alarm alarm = _alarms.get(id);
        if (alarm == null) {
            ErrorCode.writeWithMinorOpcode(client, (byte) (ErrorBase + ERROR_ALARM),
                    (short) OP_QUERY_ALARM, opcode, id);
            return;
        }

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(2);                              // reply length = 8 extra bytes / 4 = 2
            // TRIGGER: counter(4) + valueType(4) + value(8) + testType(4) = 20
            io.writeInt(alarm.counterId);
            io.writeInt(alarm.valueType);
            io.writeInt((int) (alarm.value >> 32));       // value hi
            io.writeInt((int) alarm.value);               // value lo
            io.writeInt(alarm.testType);
            // delta INT64
            io.writeInt((int) (alarm.delta >> 32));       // delta hi
            io.writeInt((int) alarm.delta);               // delta lo
            io.writeByte((byte) (alarm.events ? 1 : 0));
            io.writeByte((byte) alarm.state);
            io.writePadBytes(2);
        }
        io.flush();
    }

    // ---- DestroyAlarm (opcode 11) ----
    // Request: 4 ALARM id. No reply (void).
    private static void processDestroyAlarm(XServer xServer, Client client, InputOutput io,
                                             byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining != 4) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int id = io.readInt();

        Alarm alarm = _alarms.remove(id);
        if (alarm == null) {
            ErrorCode.writeWithMinorOpcode(client, (byte) (ErrorBase + ERROR_ALARM),
                    (short) OP_DESTROY_ALARM, opcode, id);
            return;
        }

        // Send AlarmNotify with state=Destroyed if events enabled
        if (alarm.events) {
            long counterValue = getCounterValue(xServer, alarm.counterId);
            alarm.state = ALARM_DESTROYED;
            sendAlarmNotify(alarm, counterValue);
        }
    }

    // ---- SetPriority (opcode 12) ----
    // Request: 4 XID (0=self), 4 INT32 priority. No reply.
    private static void processSetPriority(Client client, InputOutput io,
                                            byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining != 8) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int id = io.readInt();
        int priority = io.readInt();
        if (id == 0) id = client.getSequenceNumber(); // self
        _priorities.put(id, priority);
    }

    // ---- GetPriority (opcode 13) ----
    // Request: 4 XID. Reply: 4 INT32 priority, 20 unused.
    private static void processGetPriority(Client client, InputOutput io,
                                            byte opcode, int bytesRemaining) throws IOException {
        if (bytesRemaining != 4) {
            io.readSkip(bytesRemaining);
            ErrorCode.write(client, ErrorCode.Length, opcode, 0);
            return;
        }
        int id = io.readInt();
        if (id == 0) id = client.getSequenceNumber();
        int priority = _priorities.containsKey(id) ? _priorities.get(id) : 0;

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(0);             // reply length
            io.writeInt(priority);
            io.writePadBytes(20);
        }
        io.flush();
    }

    // ---- Helpers ----

    /**
     * Read alarm attribute values from the value list based on mask bits.
     * Returns remaining bytes.
     */
    private static int readAlarmValues(InputOutput io, Alarm alarm, int mask,
                                        int bytesRemaining) throws IOException {
        if ((mask & CA_COUNTER) != 0) {
            alarm.counterId = io.readInt();
            bytesRemaining -= 4;
        }
        if ((mask & CA_VALUE_TYPE) != 0) {
            alarm.valueType = io.readInt();
            bytesRemaining -= 4;
        }
        if ((mask & CA_VALUE) != 0) {
            int hi = io.readInt();
            int lo = io.readInt();
            alarm.value = ((long) hi << 32) | (lo & 0xFFFFFFFFL);
            bytesRemaining -= 8;
        }
        if ((mask & CA_TEST_TYPE) != 0) {
            alarm.testType = io.readInt();
            bytesRemaining -= 4;
        }
        if ((mask & CA_DELTA) != 0) {
            int hi = io.readInt();
            int lo = io.readInt();
            alarm.delta = ((long) hi << 32) | (lo & 0xFFFFFFFFL);
            bytesRemaining -= 8;
        }
        if ((mask & CA_EVENTS) != 0) {
            alarm.events = (io.readInt() != 0);
            bytesRemaining -= 4;
        }
        return bytesRemaining;
    }

    /**
     * Get the current value of a counter (client-created or system).
     */
    private static long getCounterValue(XServer xServer, int counterId) {
        for (SystemCounter sc : _systemCounters) {
            if (sc.id == counterId) {
                return getSystemCounterValue(sc, xServer);
            }
        }
        Counter c = _counters.get(counterId);
        return (c != null) ? c.value : 0;
    }

    /**
     * Get the current value of a system counter.
     */
    private static long getSystemCounterValue(SystemCounter sc, XServer xServer) {
        if ("SERVERTIME".equals(new String(sc.name))) {
            return xServer.getTimestamp();
        }
        return 0;
    }

    /**
     * Check all alarms attached to a counter after its value changes.
     */
    private static void checkAlarms(Counter counter, long oldValue) {
        for (Alarm alarm : _alarms.values()) {
            if (alarm.counterId == counter.id && alarm.state == ALARM_ACTIVE) {
                if (testAlarmCondition(alarm, oldValue, counter.value)) {
                    fireAlarm(alarm, counter.value);
                }
            }
        }
    }

    /**
     * Check if a single alarm should fire (used after create/change).
     */
    private static void checkSingleAlarm(XServer xServer, Alarm alarm) {
        if (alarm.state != ALARM_ACTIVE || alarm.counterId == 0) return;
        long currentValue = getCounterValue(xServer, alarm.counterId);
        // For initial check, use comparison (not transition)
        if (testComparison(alarm.testType, currentValue, alarm.value)) {
            fireAlarm(alarm, currentValue);
        }
    }

    /**
     * Test whether an alarm's trigger condition is met for a value transition.
     */
    private static boolean testAlarmCondition(Alarm alarm, long oldValue, long newValue) {
        switch (alarm.testType) {
            case POSITIVE_TRANSITION:
                // Old < threshold AND new >= threshold
                return oldValue < alarm.value && newValue >= alarm.value;
            case NEGATIVE_TRANSITION:
                // Old >= threshold AND new < threshold
                return oldValue >= alarm.value && newValue < alarm.value;
            case POSITIVE_COMPARISON:
                return newValue >= alarm.value;
            case NEGATIVE_COMPARISON:
                return newValue < alarm.value;
            default:
                return false;
        }
    }

    /**
     * Simple comparison test (for initial alarm check after create/change).
     */
    private static boolean testComparison(int testType, long value, long threshold) {
        switch (testType) {
            case POSITIVE_TRANSITION:
            case POSITIVE_COMPARISON:
                return value >= threshold;
            case NEGATIVE_TRANSITION:
            case NEGATIVE_COMPARISON:
                return value < threshold;
            default:
                return false;
        }
    }

    /**
     * Fire an alarm: send AlarmNotify event and auto-update threshold.
     */
    private static void fireAlarm(Alarm alarm, long counterValue) {
        if (alarm.events) {
            sendAlarmNotify(alarm, counterValue);
        }
        // Auto-update: add delta to threshold
        if (alarm.delta != 0) {
            alarm.value += alarm.delta;
        } else {
            alarm.state = ALARM_INACTIVE;
        }
    }

    /**
     * Send an AlarmNotify event (EventBase + 1) to the alarm's creator.
     *
     * Wire format (32 bytes):
     *   1 CARD8  type = EventBase + 1
     *   1 CARD8  kind = 1 (AlarmNotify)
     *   2 CARD16 sequence number
     *   4 ALARM  alarm id
     *   8 INT64  counter value
     *   8 INT64  alarm value (threshold)
     *   4 CARD32 timestamp
     *   1 CARD8  state
     *   3 unused
     */
    private static void sendAlarmNotify(Alarm alarm, long counterValue) {
        Client client = alarm.creator;
        if (client == null) return;

        try {
            InputOutput io = client.getInputOutput();
            synchronized (io) {
                io.writeByte((byte) (EventBase + 1));  // AlarmNotify event type
                io.writeByte((byte) 1);                 // kind = AlarmNotify
                io.writeShort((short) (client.getSequenceNumber() & 0xffff));
                io.writeInt(alarm.id);
                // counter value INT64
                io.writeInt((int) (counterValue >> 32));
                io.writeInt((int) counterValue);
                // alarm value (threshold) INT64
                io.writeInt((int) (alarm.value >> 32));
                io.writeInt((int) alarm.value);
                // timestamp
                io.writeInt(0); // We don't have xServer ref here, use 0
                io.writeByte((byte) alarm.state);
                io.writePadBytes(3);
            }
            io.flush();
        } catch (IOException e) {
            // Client disconnected
        }
    }

    // ---- Inner classes ----

    private static class SystemCounter {
        final int id;
        final int resHi;
        final int resLo;
        final byte[] name;

        SystemCounter(int id, int resHi, int resLo, String name) {
            this.id = id;
            this.resHi = resHi;
            this.resLo = resLo;
            this.name = name.getBytes();
        }
    }

    private static class Counter {
        final int id;
        long value;
        final Client creator;

        Counter(int id, long value, Client creator) {
            this.id = id;
            this.value = value;
            this.creator = creator;
        }
    }

    private static class Alarm {
        final int id;
        final Client creator;
        int counterId;
        int valueType;   // 0=Absolute, 1=Relative
        long value;      // trigger threshold
        int testType;    // 0=PosTrans, 1=NegTrans, 2=PosComp, 3=NegComp
        long delta;      // auto-increment after fire
        boolean events;  // send AlarmNotify?
        int state;       // 0=Active, 1=Inactive, 2=Destroyed

        Alarm(int id, Client creator) {
            this.id = id;
            this.creator = creator;
        }
    }
}
