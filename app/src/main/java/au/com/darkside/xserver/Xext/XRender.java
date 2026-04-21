package au.com.darkside.xserver.Xext;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.graphics.PorterDuffXfermode;
import android.graphics.Rect;
import android.util.Log;

import java.io.IOException;
import java.util.Hashtable;

import au.com.darkside.xserver.Client;
import au.com.darkside.xserver.ErrorCode;
import au.com.darkside.xserver.InputOutput;
import au.com.darkside.xserver.Util;
import au.com.darkside.xserver.XServer;

/**
 * Implementation of the X Render extension.
 * Provides compositing, anti-aliased text, and alpha blending for GTK apps.
 */
public class XRender {
    private static final String TAG = "XRender";

    // Protocol version
    private static final int MAJOR_VERSION = 0;
    private static final int MINOR_VERSION = 11;

    // Minor opcodes — per xcb-proto render.xml spec
    private static final int QueryVersion = 0;
    private static final int QueryPictFormats = 1;
    private static final int QueryPictIndexValues = 2;
    // opcode 3 reserved for QueryDithers
    private static final int CreatePicture = 4;
    private static final int ChangePicture = 5;
    private static final int SetPictureClipRectangles = 6;
    private static final int FreePicture = 7;
    private static final int Composite = 8;
    // opcode 9 reserved for Scale
    private static final int Trapezoids = 10;
    private static final int Triangles = 11;
    private static final int TriStrip = 12;
    private static final int TriFan = 13;
    // opcodes 14-16 reserved
    private static final int CreateGlyphSet = 17;
    private static final int ReferenceGlyphSet = 18;
    private static final int FreeGlyphSet = 19;
    private static final int AddGlyphs = 20;
    // opcode 21 reserved
    private static final int FreeGlyphs = 22;
    private static final int CompositeGlyphs8 = 23;
    private static final int CompositeGlyphs16 = 24;
    private static final int CompositeGlyphs32 = 25;
    private static final int FillRectangles = 26;
    private static final int CreateCursor = 27;
    private static final int SetPictureTransform = 28;
    private static final int QueryFilters = 29;
    private static final int SetPictureFilter = 30;
    private static final int CreateAnimCursor = 31;
    private static final int AddTraps = 32;
    private static final int CreateSolidFill = 33;
    private static final int CreateLinearGradient = 34;
    private static final int CreateRadialGradient = 35;
    private static final int CreateConicalGradient = 36;

    // PictOp operators
    private static final int PictOpClear = 0;
    private static final int PictOpSrc = 1;
    private static final int PictOpDst = 2;
    private static final int PictOpOver = 3;
    private static final int PictOpOverReverse = 4;
    private static final int PictOpIn = 5;
    private static final int PictOpInReverse = 6;
    private static final int PictOpOut = 7;
    private static final int PictOpOutReverse = 8;
    private static final int PictOpAtop = 9;
    private static final int PictOpAtopReverse = 10;
    private static final int PictOpXor = 11;
    private static final int PictOpAdd = 12;
    private static final int PictOpSaturate = 13;

    // PictFormat IDs
    private static final int FMT_ARGB32 = 100;
    private static final int FMT_RGB24 = 101;
    private static final int FMT_A8 = 102;
    private static final int FMT_A4 = 103;
    private static final int FMT_A1 = 104;

    // Error base
    public static final byte ErrorBase = (byte) 140;

    // Picture storage
    private static final Hashtable<Integer, Picture> _pictures = new Hashtable<>();
    private static final Hashtable<Integer, GlyphSet> _glyphSets = new Hashtable<>();
    private static final Hashtable<Integer, Bitmap> _solidFills = new Hashtable<>();

    /**
     * A Picture wraps a Drawable with a format and rendering attributes.
     */
    private static class Picture {
        int id;
        int drawableId;
        int formatId;
        Bitmap bitmap;
        boolean hasAlpha;
        int repeatMode = 0; // 0=None, 1=Normal, 2=Pad, 3=Reflect
        boolean componentAlpha = false;
        Rect clipRect = null;

        Picture(int id, int drawableId, int formatId, Bitmap bitmap, boolean hasAlpha) {
            this.id = id;
            this.drawableId = drawableId;
            this.formatId = formatId;
            this.bitmap = bitmap;
            this.hasAlpha = hasAlpha;
        }
    }

    /**
     * A GlyphSet holds a collection of glyph images for text rendering.
     */
    private static class GlyphSet {
        int id;
        int formatId;
        Hashtable<Integer, Glyph> glyphs = new Hashtable<>();

        GlyphSet(int id, int formatId) {
            this.id = id;
            this.formatId = formatId;
        }
    }

    private static class Glyph {
        int width, height;
        int originX, originY;
        int advanceX, advanceY;
        Bitmap bitmap;
    }

    /**
     * Process a RENDER extension request.
     */
    public static void processRequest(XServer xServer, Client client, byte majorOpcode, byte minorOpcode, int bytesRemaining) throws IOException {
        InputOutput io = client.getInputOutput();

        Log.d(TAG, "Request minor=" + (minorOpcode & 0xff) + " bytes=" + bytesRemaining);
        switch (minorOpcode & 0xff) {
            case QueryVersion:
                processQueryVersion(client, io, bytesRemaining);
                break;
            case QueryPictFormats:
                processQueryPictFormats(client, io, xServer, bytesRemaining);
                break;
            case QueryPictIndexValues:
                io.readSkip(bytesRemaining);
                // Return empty list
                synchronized (io) {
                    Util.writeReplyHeader(client, (byte) 0);
                    io.writeInt(0);
                    io.writeInt(0); // num values
                    io.writePadBytes(20);
                }
                io.flush();
                break;
            case QueryFilters:
                processQueryFilters(client, io, bytesRemaining);
                break;
            case CreatePicture:
                processCreatePicture(xServer, client, io, bytesRemaining);
                break;
            case ChangePicture:
                processChangePicture(client, io, bytesRemaining);
                break;
            case SetPictureClipRectangles:
                processSetPictureClipRectangles(client, io, bytesRemaining);
                break;
            case FreePicture:
                processFreePicture(client, io, bytesRemaining);
                break;
            case Composite:
                processComposite(xServer, client, io, bytesRemaining);
                break;
            case FillRectangles:
                processFillRectangles(client, io, bytesRemaining);
                break;
            case CreateGlyphSet:
                processCreateGlyphSet(client, io, bytesRemaining);
                break;
            case ReferenceGlyphSet:
                processReferenceGlyphSet(client, io, bytesRemaining);
                break;
            case FreeGlyphSet:
                processFreeGlyphSet(client, io, bytesRemaining);
                break;
            case AddGlyphs:
                processAddGlyphs(client, io, bytesRemaining);
                break;
            case FreeGlyphs:
                processFreeGlyphs(client, io, bytesRemaining);
                break;
            case CompositeGlyphs8:
            case CompositeGlyphs16:
            case CompositeGlyphs32:
                processCompositeGlyphs(xServer, client, io, bytesRemaining, minorOpcode & 0xff);
                break;
            case Trapezoids:
            case Triangles:
            case TriStrip:
            case TriFan:
                // Skip geometric operations for now
                io.readSkip(bytesRemaining);
                break;
            case SetPictureTransform:
                // Read and ignore the 3x3 matrix (36 bytes)
                io.readSkip(bytesRemaining);
                break;
            case SetPictureFilter:
                io.readSkip(bytesRemaining);
                break;
            case CreateSolidFill:
                processCreateSolidFill(client, io, bytesRemaining);
                break;
            case CreateLinearGradient:
            case CreateRadialGradient:
            case CreateConicalGradient:
                processCreateGradient(client, io, bytesRemaining);
                break;
            case CreateCursor:
                processCreateCursor(xServer, client, io, bytesRemaining);
                break;
            case CreateAnimCursor:
                processCreateAnimCursor(xServer, client, io, bytesRemaining);
                break;
            case AddTraps:
                io.readSkip(bytesRemaining);
                break;
            default:
                io.readSkip(bytesRemaining);
                break;
        }
    }

    private static void processQueryVersion(Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 8) {
            io.readSkip(bytesRemaining);
            return;
        }
        int clientMajor = io.readInt();
        int clientMinor = io.readInt();
        bytesRemaining -= 8;
        io.readSkip(bytesRemaining);

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(0); // reply length
            io.writeInt(MAJOR_VERSION);
            io.writeInt(MINOR_VERSION);
            io.writePadBytes(16);
        }
        io.flush();
    }

    private static void processQueryPictFormats(Client client, InputOutput io, XServer xServer, int bytesRemaining) throws IOException {
        io.readSkip(bytesRemaining);

        // Define 5 standard formats
        int numFormats = 5;
        int numScreens = 1;
        int numDepths = 4; // depth 1, 8, 24, 32
        int numVisuals = 2; // visual 1 at depth 24 + visual 1 at depth 32

        // Calculate reply length
        // Formats: 5 * 28 = 140
        // Screen header: 8
        // Depth 1: 8 (no visuals)
        // Depth 8: 8 (no visuals)
        // Depth 24: 8 + 1*8 = 16 (visual 1 -> RGB24)
        // Depth 32: 8 + 1*8 = 16 (visual 1 -> ARGB32)
        // Subpixel: 4
        int totalBytes = 140 + 8 + 8 + 8 + 16 + 16 + 4;  // = 200
        int replyLength = totalBytes / 4;  // = 50

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(replyLength);
            io.writeInt(numFormats);   // num formats
            io.writeInt(numScreens);   // num screens
            io.writeInt(numDepths);    // num depths
            io.writeInt(numVisuals);   // num visuals
            io.writeInt(numScreens);   // num subpixel = num screens
            io.writePadBytes(4);

            // Format: ARGB32 (depth 32, with alpha)
            writePictFormat(io, FMT_ARGB32, 1, 32, 16, 0xff, 8, 0xff, 0, 0xff, 24, 0xff);
            // Format: RGB24 (depth 24, no alpha)
            writePictFormat(io, FMT_RGB24, 1, 24, 16, 0xff, 8, 0xff, 0, 0xff, 0, 0x00);
            // Format: A8 (depth 8, alpha only)
            writePictFormat(io, FMT_A8, 1, 8, 0, 0x00, 0, 0x00, 0, 0x00, 0, 0xff);
            // Format: A4 (depth 4)
            writePictFormat(io, FMT_A4, 1, 4, 0, 0x00, 0, 0x00, 0, 0x00, 0, 0x0f);
            // Format: A1 (depth 1)
            writePictFormat(io, FMT_A1, 1, 1, 0, 0x00, 0, 0x00, 0, 0x00, 0, 0x01);

            // Screen 0
            io.writeInt(numDepths); // num depths
            io.writeInt(FMT_RGB24); // fallback format for this screen

            // Depth 1: 0 visuals
            io.writeByte((byte) 1);
            io.writeByte((byte) 0);
            io.writeShort((short) 0);
            io.writePadBytes(4);

            // Depth 8: 0 visuals
            io.writeByte((byte) 8);
            io.writeByte((byte) 0);
            io.writeShort((short) 0);
            io.writePadBytes(4);

            // Depth 24: visual 1 -> RGB24 (THIS IS THE KEY — root visual is depth 24)
            io.writeByte((byte) 24);
            io.writeByte((byte) 0);
            io.writeShort((short) 1); // 1 visual
            io.writePadBytes(4);
            io.writeInt(1);           // visual id = 1
            io.writeInt(FMT_RGB24);   // format = RGB24

            // Depth 32: visual 1 -> ARGB32 (for alpha-capable windows)
            io.writeByte((byte) 32);
            io.writeByte((byte) 0);
            io.writeShort((short) 1); // 1 visual
            io.writePadBytes(4);
            io.writeInt(1);           // visual id = 1
            io.writeInt(FMT_ARGB32);  // format = ARGB32

            // Subpixel order (0 = Unknown)
            io.writeInt(0);
        }
        io.flush();
    }

    private static void writePictFormat(InputOutput io, int id, int type, int depth,
            int redShift, int redMask, int greenShift, int greenMask,
            int blueShift, int blueMask, int alphaShift, int alphaMask) throws IOException {
        io.writeInt(id);          // format id
        io.writeByte((byte) type); // type (1=Direct)
        io.writeByte((byte) depth);
        io.writeShort((short) 0);  // pad
        io.writeShort((short) redShift);
        io.writeShort((short) redMask);
        io.writeShort((short) greenShift);
        io.writeShort((short) greenMask);
        io.writeShort((short) blueShift);
        io.writeShort((short) blueMask);
        io.writeShort((short) alphaShift);
        io.writeShort((short) alphaMask);
        io.writeInt(0); // colormap (0 for direct)
    }

    private static void processQueryFilters(Client client, InputOutput io, int bytesRemaining) throws IOException {
        io.readSkip(bytesRemaining);
        // Return two standard filters: "nearest" and "bilinear"
        byte[] nearest = "nearest".getBytes();
        byte[] bilinear = "bilinear".getBytes();
        int stringBytes = 1 + nearest.length + 1 + bilinear.length;
        int pad = (-stringBytes) & 3;
        int aliasBytes = 2 * 2; // 2 aliases, 2 bytes each
        int totalBytes = stringBytes + pad + aliasBytes;
        int replyLength = (totalBytes + 3) / 4;

        synchronized (io) {
            Util.writeReplyHeader(client, (byte) 0);
            io.writeInt(replyLength);
            io.writeInt(2); // num aliases
            io.writeInt(2); // num filters
            io.writePadBytes(16);
            // Aliases
            io.writeShort((short) 0); // alias 0 -> filter 0
            io.writeShort((short) 1); // alias 1 -> filter 1
            // Filter names (Pascal strings: length byte + chars)
            io.writeByte((byte) nearest.length);
            io.writeBytes(nearest, 0, nearest.length);
            io.writeByte((byte) bilinear.length);
            io.writeBytes(bilinear, 0, bilinear.length);
            io.writePadBytes(pad);
        }
        io.flush();
    }

    private static void processCreatePicture(XServer xServer, Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 12) {
            io.readSkip(bytesRemaining);
            return;
        }
        int pictId = io.readInt();
        int drawableId = io.readInt();
        int formatId = io.readInt();
        bytesRemaining -= 12;

        // Read value mask and values
        int valueMask = 0;
        if (bytesRemaining >= 4) {
            valueMask = io.readInt();
            bytesRemaining -= 4;
        }

        int repeat = 0;
        boolean componentAlpha = false;

        // Parse value list based on mask
        for (int bit = 0; bit < 16 && bytesRemaining >= 4; bit++) {
            if ((valueMask & (1 << bit)) != 0) {
                int val = io.readInt();
                bytesRemaining -= 4;
                if (bit == 0) repeat = val;      // Repeat
                if (bit == 11) componentAlpha = (val != 0); // ComponentAlpha
            }
        }
        io.readSkip(bytesRemaining);

        boolean hasAlpha = (formatId == FMT_ARGB32 || formatId == FMT_A8 || formatId == FMT_A4 || formatId == FMT_A1);

        // Try to get the bitmap from the drawable
        Bitmap bmp = null;
        if (xServer.resourceExists(drawableId)) {
            au.com.darkside.xserver.Resource res = xServer.getResource(drawableId);
            if (res instanceof au.com.darkside.xserver.Pixmap) {
                bmp = ((au.com.darkside.xserver.Pixmap) res).getDrawable().getBitmap();
            } else if (res instanceof au.com.darkside.xserver.Window) {
                bmp = ((au.com.darkside.xserver.Window) res).getDrawable().getBitmap();
            }
        }

        if (bmp == null) {
            // Create a 1x1 fallback
            bmp = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888);
        }

        Picture pic = new Picture(pictId, drawableId, formatId, bmp, hasAlpha);
        pic.repeatMode = repeat;
        pic.componentAlpha = componentAlpha;
        _pictures.put(pictId, pic);
    }

    private static void processChangePicture(Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 8) {
            io.readSkip(bytesRemaining);
            return;
        }
        int pictId = io.readInt();
        int valueMask = io.readInt();
        bytesRemaining -= 8;

        Picture pic = _pictures.get(pictId);

        for (int bit = 0; bit < 16 && bytesRemaining >= 4; bit++) {
            if ((valueMask & (1 << bit)) != 0) {
                int val = io.readInt();
                bytesRemaining -= 4;
                if (pic != null) {
                    if (bit == 0) pic.repeatMode = val;
                    if (bit == 11) pic.componentAlpha = (val != 0);
                }
            }
        }
        io.readSkip(bytesRemaining);
    }

    private static void processSetPictureClipRectangles(Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 8) {
            io.readSkip(bytesRemaining);
            return;
        }
        int pictId = io.readInt();
        int clipXOrigin = (short) io.readShort();
        int clipYOrigin = (short) io.readShort();
        bytesRemaining -= 8;

        Picture pic = _pictures.get(pictId);
        // Read first rectangle as clip
        if (pic != null && bytesRemaining >= 8) {
            int x = (short) io.readShort();
            int y = (short) io.readShort();
            int w = io.readShort();
            int h = io.readShort();
            bytesRemaining -= 8;
            pic.clipRect = new Rect(clipXOrigin + x, clipYOrigin + y, clipXOrigin + x + w, clipYOrigin + y + h);
        }
        io.readSkip(bytesRemaining);
    }

    private static void processFreePicture(Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 4) {
            io.readSkip(bytesRemaining);
            return;
        }
        int pictId = io.readInt();
        bytesRemaining -= 4;
        io.readSkip(bytesRemaining);
        _pictures.remove(pictId);
    }

    private static void processComposite(XServer xServer, Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 32) {
            io.readSkip(bytesRemaining);
            return;
        }

        int op = io.readByte() & 0xff;
        io.readSkip(3); // pad
        int srcId = io.readInt();
        int maskId = io.readInt();
        int dstId = io.readInt();
        int srcX = (short) io.readShort();
        int srcY = (short) io.readShort();
        int maskX = (short) io.readShort();
        int maskY = (short) io.readShort();
        int dstX = (short) io.readShort();
        int dstY = (short) io.readShort();
        int width = io.readShort() & 0xffff;
        int height = io.readShort() & 0xffff;
        bytesRemaining -= 32;
        io.readSkip(bytesRemaining);

        Picture src = _pictures.get(srcId);
        Picture dst = _pictures.get(dstId);
        Picture mask = (maskId != 0) ? _pictures.get(maskId) : null;

        if (src == null || dst == null) {
            Log.e(TAG, "Composite: src=" + (src != null) + " dst=" + (dst != null) + " srcId=" + srcId + " dstId=" + dstId);
            return;
        }
        if (dst.bitmap == null) {
            Log.e(TAG, "Composite: dst bitmap null, drawableId=" + dst.drawableId);
            return;
        }
        if (width == 0 || height == 0) return;

        try {
            Bitmap dstBmp = dst.bitmap;
            if (!dstBmp.isMutable()) return;

            // Per XRender spec: result = (src IN mask) OP dst
            // If mask is present, first composite src through mask, then apply to dst
            Bitmap srcBmp = src.bitmap;
            if (srcBmp == null || srcBmp.getWidth() == 0 || srcBmp.getHeight() == 0) return;

            // Step 1: Resolve the effective source (apply mask if present)
            Bitmap effectiveSrc = null;
            boolean tempSrc = false;

            if (mask != null && mask.bitmap != null && mask.bitmap.getWidth() > 0 && mask.bitmap.getHeight() > 0) {
                // Create temp bitmap: src IN mask
                effectiveSrc = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
                tempSrc = true;
                Canvas tmpCanvas = new Canvas(effectiveSrc);

                // Draw source into temp
                Paint srcPaint = new Paint(Paint.FILTER_BITMAP_FLAG);
                drawPictureRegion(tmpCanvas, src, srcX, srcY, 0, 0, width, height, srcPaint);

                // Apply mask with DST_IN: keeps dst (src pixels) where mask alpha is nonzero
                Paint maskPaint = new Paint(Paint.FILTER_BITMAP_FLAG);
                maskPaint.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.DST_IN));
                drawPictureRegion(tmpCanvas, mask, maskX, maskY, 0, 0, width, height, maskPaint);
            }

            // Step 2: Composite (effective) source onto destination
            Canvas canvas = new Canvas(dstBmp);
            Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
            paint.setXfermode(getPorterDuffMode(op));

            if (tempSrc) {
                // Draw the pre-masked source onto dst
                canvas.drawBitmap(effectiveSrc, dstX, dstY, paint);
                effectiveSrc.recycle();
            } else {
                // No mask — draw source directly
                drawPictureRegion(canvas, src, srcX, srcY, dstX, dstY, width, height, paint);
            }

            // Invalidate the destination window if it's on screen
            if (xServer.resourceExists(dst.drawableId)) {
                au.com.darkside.xserver.Resource res = xServer.getResource(dst.drawableId);
                if (res instanceof au.com.darkside.xserver.Window) {
                    ((au.com.darkside.xserver.Window) res).invalidate(dstX, dstY, width, height);
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "Composite error", e);
        }
    }

    private static void processFillRectangles(Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 16) {
            io.readSkip(bytesRemaining);
            return;
        }

        int op = io.readByte() & 0xff;
        io.readSkip(3); // pad
        int dstId = io.readInt();
        // Color: RGBA 16-bit each
        int red = io.readShort() & 0xffff;
        int green = io.readShort() & 0xffff;
        int blue = io.readShort() & 0xffff;
        int alpha = io.readShort() & 0xffff;
        bytesRemaining -= 16;

        Picture dst = _pictures.get(dstId);
        if (dst == null || dst.bitmap == null || !dst.bitmap.isMutable()) {
            io.readSkip(bytesRemaining);
            return;
        }

        int color = Color.argb(alpha >> 8, red >> 8, green >> 8, blue >> 8);

        Canvas canvas = new Canvas(dst.bitmap);
        Paint paint = new Paint();
        paint.setColor(color);
        paint.setXfermode(getPorterDuffMode(op));

        while (bytesRemaining >= 8) {
            int x = (short) io.readShort();
            int y = (short) io.readShort();
            int w = io.readShort() & 0xffff;
            int h = io.readShort() & 0xffff;
            bytesRemaining -= 8;
            canvas.drawRect(x, y, x + w, y + h, paint);
        }
        io.readSkip(bytesRemaining);
    }

    private static void processCreateGlyphSet(Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 8) {
            io.readSkip(bytesRemaining);
            return;
        }
        int gsId = io.readInt();
        int formatId = io.readInt();
        bytesRemaining -= 8;
        io.readSkip(bytesRemaining);
        _glyphSets.put(gsId, new GlyphSet(gsId, formatId));
    }

    private static void processReferenceGlyphSet(Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 8) {
            io.readSkip(bytesRemaining);
            return;
        }
        int gsId = io.readInt();
        int existingId = io.readInt();
        bytesRemaining -= 8;
        io.readSkip(bytesRemaining);

        GlyphSet existing = _glyphSets.get(existingId);
        if (existing != null) {
            GlyphSet ref = new GlyphSet(gsId, existing.formatId);
            ref.glyphs = existing.glyphs; // share glyphs
            _glyphSets.put(gsId, ref);
        }
    }

    private static void processFreeGlyphSet(Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 4) {
            io.readSkip(bytesRemaining);
            return;
        }
        int gsId = io.readInt();
        bytesRemaining -= 4;
        io.readSkip(bytesRemaining);
        _glyphSets.remove(gsId);
    }

    private static void processAddGlyphs(Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 8) {
            io.readSkip(bytesRemaining);
            return;
        }

        int gsId = io.readInt();
        int numGlyphs = io.readInt();
        bytesRemaining -= 8;

        GlyphSet gs = _glyphSets.get(gsId);

        // Per spec: ALL glyph IDs first
        int[] glyphIds = new int[numGlyphs];
        for (int i = 0; i < numGlyphs && bytesRemaining >= 4; i++) {
            glyphIds[i] = io.readInt();
            bytesRemaining -= 4;
        }

        // Then ALL GLYPHINFO structs (12 bytes each)
        int[] widths = new int[numGlyphs];
        int[] heights = new int[numGlyphs];
        int[] originXs = new int[numGlyphs];
        int[] originYs = new int[numGlyphs];
        int[] advanceXs = new int[numGlyphs];
        int[] advanceYs = new int[numGlyphs];
        for (int i = 0; i < numGlyphs && bytesRemaining >= 12; i++) {
            widths[i] = io.readShort() & 0xffff;
            heights[i] = io.readShort() & 0xffff;
            originXs[i] = (short) io.readShort();
            originYs[i] = (short) io.readShort();
            advanceXs[i] = (short) io.readShort();
            advanceYs[i] = (short) io.readShort();
            bytesRemaining -= 12;
        }

        // Determine glyph pixel depth from format
        int glyphDepth = 8; // default A8
        if (gs != null) {
            switch (gs.formatId) {
                case FMT_ARGB32: glyphDepth = 32; break;
                case FMT_A8: glyphDepth = 8; break;
                case FMT_A4: glyphDepth = 4; break;
                case FMT_A1: glyphDepth = 1; break;
            }
        }

        // Then ALL pixel data (Z-format images, each padded to 32-bit boundary)
        for (int i = 0; i < numGlyphs; i++) {
            int w = widths[i];
            int h = heights[i];
            // Per spec: Z-format image with rows padded to 32-bit boundary
            // stride = PixmapBytePad(width, depth) = ((width * depth + 31) / 32) * 4
            int stride = ((w * glyphDepth + 31) / 32) * 4;
            int dataLen = stride * h;
            int padLen = ((-dataLen) & 3);

            if (gs != null && w > 0 && h > 0 && bytesRemaining >= dataLen + padLen) {
                Glyph g = new Glyph();
                g.width = w;
                g.height = h;
                g.originX = originXs[i];
                g.originY = originYs[i];
                g.advanceX = advanceXs[i];
                g.advanceY = advanceYs[i];

                // Create ARGB bitmap from glyph alpha data
                int[] pixels = new int[w * h];

                if (glyphDepth == 8) {
                    // A8: 1 byte per pixel, with row stride padding
                    for (int y = 0; y < h; y++) {
                        for (int x = 0; x < w; x++) {
                            int a = io.readByte() & 0xff;
                            pixels[y * w + x] = (a << 24) | 0xFFFFFF;
                        }
                        // Skip row padding bytes
                        io.readSkip(stride - w);
                    }
                } else if (glyphDepth == 1) {
                    // A1: 1 bit per pixel, rows padded to 4 bytes
                    for (int y = 0; y < h; y++) {
                        int bytesPerRow = (w + 7) / 8;
                        for (int byteIdx = 0; byteIdx < bytesPerRow; byteIdx++) {
                            int val = io.readByte() & 0xff;
                            for (int bit = 0; bit < 8 && byteIdx * 8 + bit < w; bit++) {
                                int a = ((val >> bit) & 1) == 1 ? 0xFF : 0x00;
                                pixels[y * w + byteIdx * 8 + bit] = (a << 24) | 0xFFFFFF;
                            }
                        }
                        io.readSkip(stride - bytesPerRow);
                    }
                } else if (glyphDepth == 32) {
                    // ARGB32: 4 bytes per pixel
                    for (int y = 0; y < h; y++) {
                        for (int x = 0; x < w; x++) {
                            pixels[y * w + x] = io.readInt();
                        }
                    }
                } else {
                    // A4 or other: skip for now
                    io.readSkip(dataLen);
                }
                bytesRemaining -= dataLen;
                io.readSkip(padLen);
                bytesRemaining -= padLen;

                g.bitmap = Bitmap.createBitmap(pixels, w, h, Bitmap.Config.ARGB_8888);
                gs.glyphs.put(glyphIds[i], g);
            } else if (dataLen + padLen > 0) {
                int skip = Math.min(dataLen + padLen, bytesRemaining);
                io.readSkip(skip);
                bytesRemaining -= skip;
            }
        }
        io.readSkip(bytesRemaining);
    }

    private static void processFreeGlyphs(Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 4) {
            io.readSkip(bytesRemaining);
            return;
        }
        int gsId = io.readInt();
        bytesRemaining -= 4;

        GlyphSet gs = _glyphSets.get(gsId);
        while (bytesRemaining >= 4) {
            int glyphId = io.readInt();
            bytesRemaining -= 4;
            if (gs != null) gs.glyphs.remove(glyphId);
        }
        io.readSkip(bytesRemaining);
    }

    private static void processCompositeGlyphs(XServer xServer, Client client, InputOutput io, int bytesRemaining, int opcode) throws IOException {
        if (bytesRemaining < 24) {
            io.readSkip(bytesRemaining);
            return;
        }

        int op = io.readByte() & 0xff;
        io.readSkip(3); // pad
        int srcId = io.readInt();
        int dstId = io.readInt();
        int maskFormat = io.readInt();
        int gsId = io.readInt();
        int srcX = (short) io.readShort();
        int srcY = (short) io.readShort();
        bytesRemaining -= 24;

        Picture src = _pictures.get(srcId);
        Picture dst = _pictures.get(dstId);
        GlyphSet gs = _glyphSets.get(gsId);

        if (dst == null || dst.bitmap == null || !dst.bitmap.isMutable() || gs == null) {
            io.readSkip(bytesRemaining);
            return;
        }

        int glyphBytes = (opcode == CompositeGlyphs8) ? 1 : (opcode == CompositeGlyphs16) ? 2 : 4;

        // Per Xorg miGlyphs: glyph bitmap is the MASK, src picture provides color
        // Formula: dst = (src IN glyph_mask) OP dst
        // Get source color for tinting glyphs
        int srcColor = 0xFFFFFFFF; // default white
        if (src != null && src.bitmap != null && src.bitmap.getWidth() > 0 && src.bitmap.getHeight() > 0) {
            srcColor = src.bitmap.getPixel(
                Math.min(Math.max(0, srcX), src.bitmap.getWidth() - 1),
                Math.min(Math.max(0, srcY), src.bitmap.getHeight() - 1));
        }

        Canvas canvas = new Canvas(dst.bitmap);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        paint.setXfermode(getPorterDuffMode(op));
        // Tint glyph (white+alpha) with source color via SRC_IN:
        // result = srcColor * glyphAlpha — exactly the XRender "src IN mask" formula
        paint.setColorFilter(new PorterDuffColorFilter(srcColor, PorterDuff.Mode.SRC_IN));

        int curX = 0, curY = 0;
        boolean firstElt = true;

        while (bytesRemaining >= 8) {
            int numGlyphs = io.readByte() & 0xff;
            io.readSkip(3); // pad
            int deltaX = (short) io.readShort();
            int deltaY = (short) io.readShort();
            bytesRemaining -= 8;

            if (numGlyphs == 0xff) {
                // Switch glyph set
                if (bytesRemaining >= 4) {
                    gsId = io.readInt();
                    bytesRemaining -= 4;
                    gs = _glyphSets.get(gsId);
                }
                continue;
            }

            if (firstElt) {
                curX = deltaX;
                curY = deltaY;
                firstElt = false;
            } else {
                curX += deltaX;
                curY += deltaY;
            }

            int dataBytes = numGlyphs * glyphBytes;
            int padBytes = ((-dataBytes) & 3);

            for (int i = 0; i < numGlyphs && bytesRemaining >= glyphBytes; i++) {
                int glyphId;
                if (glyphBytes == 1) {
                    glyphId = io.readByte() & 0xff;
                } else if (glyphBytes == 2) {
                    glyphId = io.readShort() & 0xffff;
                } else {
                    glyphId = io.readInt();
                }
                bytesRemaining -= glyphBytes;

                if (gs != null) {
                    Glyph g = gs.glyphs.get(glyphId);
                    if (g != null && g.bitmap != null) {
                        int drawX = curX - g.originX;
                        int drawY = curY - g.originY;
                        canvas.drawBitmap(g.bitmap, drawX, drawY, paint);
                        curX += g.advanceX;
                        curY += g.advanceY;
                    }
                }
            }
            io.readSkip(padBytes);
            bytesRemaining -= padBytes;
        }
        io.readSkip(bytesRemaining);

        // Invalidate destination
        if (xServer.resourceExists(dst.drawableId)) {
            au.com.darkside.xserver.Resource res = xServer.getResource(dst.drawableId);
            if (res instanceof au.com.darkside.xserver.Window) {
                ((au.com.darkside.xserver.Window) res).invalidate();
            }
        }
    }

    private static void processCreateSolidFill(Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 12) {
            io.readSkip(bytesRemaining);
            return;
        }
        int pictId = io.readInt();
        int red = io.readShort() & 0xffff;
        int green = io.readShort() & 0xffff;
        int blue = io.readShort() & 0xffff;
        int alpha = io.readShort() & 0xffff;
        bytesRemaining -= 12;
        io.readSkip(bytesRemaining);

        int color = Color.argb(alpha >> 8, red >> 8, green >> 8, blue >> 8);
        Bitmap bmp = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888);
        bmp.eraseColor(color);

        Picture pic = new Picture(pictId, 0, FMT_ARGB32, bmp, true);
        pic.repeatMode = 1; // solid fills always repeat
        _pictures.put(pictId, pic);
    }

    private static void processCreateGradient(Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 4) {
            io.readSkip(bytesRemaining);
            return;
        }
        int pictId = io.readInt();
        bytesRemaining -= 4;
        io.readSkip(bytesRemaining);

        // Create a simple gray gradient placeholder
        Bitmap bmp = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888);
        bmp.eraseColor(Color.GRAY);
        Picture pic = new Picture(pictId, 0, FMT_ARGB32, bmp, true);
        pic.repeatMode = 1;
        _pictures.put(pictId, pic);
    }

    /**
     * Draw a picture's source region onto a canvas, handling repeat modes.
     * Extracted from Composite to reuse for mask application.
     */
    private static void drawPictureRegion(Canvas canvas, Picture pic, int picX, int picY,
                                           int canvasX, int canvasY, int width, int height, Paint paint) {
        Bitmap bmp = pic.bitmap;
        int sw = bmp.getWidth();
        int sh = bmp.getHeight();

        if (pic.repeatMode != 0 && sw <= 4 && sh <= 4) {
            // Solid fill or small pattern — fill with pixel color
            int pixel = bmp.getPixel(0, 0);
            paint.setColor(pixel);
            paint.setStyle(Paint.Style.FILL);
            canvas.drawRect(canvasX, canvasY, canvasX + width, canvasY + height, paint);
        } else if (pic.repeatMode != 0) {
            // Tiling
            android.graphics.BitmapShader shader = new android.graphics.BitmapShader(
                bmp, android.graphics.Shader.TileMode.REPEAT, android.graphics.Shader.TileMode.REPEAT);
            android.graphics.Matrix m = new android.graphics.Matrix();
            m.setTranslate(canvasX - picX, canvasY - picY);
            shader.setLocalMatrix(m);
            paint.setShader(shader);
            canvas.drawRect(canvasX, canvasY, canvasX + width, canvasY + height, paint);
            paint.setShader(null);
        } else {
            // Non-repeating — copy region
            int effectiveX = Math.max(0, Math.min(picX, sw - 1));
            int effectiveY = Math.max(0, Math.min(picY, sh - 1));
            int copyW = Math.min(width, sw - effectiveX);
            int copyH = Math.min(height, sh - effectiveY);
            if (copyW > 0 && copyH > 0) {
                Rect srcRect = new Rect(effectiveX, effectiveY, effectiveX + copyW, effectiveY + copyH);
                Rect dstRect = new Rect(canvasX, canvasY, canvasX + copyW, canvasY + copyH);
                canvas.drawBitmap(bmp, srcRect, dstRect, paint);
            }
        }
    }

    private static PorterDuffXfermode getPorterDuffMode(int op) {
        switch (op) {
            case PictOpClear:    return new PorterDuffXfermode(PorterDuff.Mode.CLEAR);
            case PictOpSrc:      return new PorterDuffXfermode(PorterDuff.Mode.SRC);
            case PictOpDst:      return new PorterDuffXfermode(PorterDuff.Mode.DST);
            case PictOpOver:     return new PorterDuffXfermode(PorterDuff.Mode.SRC_OVER);
            case PictOpOverReverse: return new PorterDuffXfermode(PorterDuff.Mode.DST_OVER);
            case PictOpIn:       return new PorterDuffXfermode(PorterDuff.Mode.SRC_IN);
            case PictOpInReverse: return new PorterDuffXfermode(PorterDuff.Mode.DST_IN);
            case PictOpOut:      return new PorterDuffXfermode(PorterDuff.Mode.SRC_OUT);
            case PictOpOutReverse: return new PorterDuffXfermode(PorterDuff.Mode.DST_OUT);
            case PictOpAtop:     return new PorterDuffXfermode(PorterDuff.Mode.SRC_ATOP);
            case PictOpAtopReverse: return new PorterDuffXfermode(PorterDuff.Mode.DST_ATOP);
            case PictOpXor:      return new PorterDuffXfermode(PorterDuff.Mode.XOR);
            case PictOpAdd:      return new PorterDuffXfermode(PorterDuff.Mode.ADD);
            default:             return new PorterDuffXfermode(PorterDuff.Mode.SRC_OVER);
        }
    }

    /**
     * CreateCursor (opcode 27): cid(4) + source(4) + x(2) + y(2) = 12 bytes
     */
    private static void processCreateCursor(XServer xServer, Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 12) {
            io.readSkip(bytesRemaining);
            return;
        }
        int cid = io.readInt();
        int sourceId = io.readInt();
        int x = io.readShort() & 0xffff;
        int y = io.readShort() & 0xffff;
        bytesRemaining -= 12;
        io.readSkip(bytesRemaining);

        // Create a cursor from the picture's bitmap
        Picture src = _pictures.get(sourceId);
        Bitmap bmp = null;
        if (src != null && src.bitmap != null) {
            bmp = src.bitmap;
        }
        if (bmp == null) {
            bmp = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888);
        }

        // Register as X cursor resource
        au.com.darkside.xserver.Cursor cursor = new au.com.darkside.xserver.Cursor(
            cid, xServer, client, bmp, x, y);
        xServer.addResource(cursor);
        if (client != null) client.addResource(cursor);
    }

    /**
     * CreateAnimCursor (opcode 31): cid(4) + cursors(LISTofANIMCURSORELT)
     */
    private static void processCreateAnimCursor(XServer xServer, Client client, InputOutput io, int bytesRemaining) throws IOException {
        if (bytesRemaining < 4) {
            io.readSkip(bytesRemaining);
            return;
        }
        int cid = io.readInt();
        bytesRemaining -= 4;

        // Read first cursor from the list as the static cursor
        int firstCursorId = 0;
        if (bytesRemaining >= 8) {
            firstCursorId = io.readInt(); // cursor ID
            io.readInt(); // delay
            bytesRemaining -= 8;
        }
        io.readSkip(bytesRemaining);

        // Use the first cursor in the animation, or create a blank one
        au.com.darkside.xserver.Resource existing = xServer.getResource(firstCursorId);
        if (existing != null && existing.getType() == au.com.darkside.xserver.Resource.CURSOR) {
            // Re-register the existing cursor with the new ID
            Bitmap bmp = ((au.com.darkside.xserver.Cursor) existing).getBitmap();
            au.com.darkside.xserver.Cursor cursor = new au.com.darkside.xserver.Cursor(
                cid, xServer, client, bmp, 0, 0);
            xServer.addResource(cursor);
            if (client != null) client.addResource(cursor);
        } else {
            // Create a blank cursor
            Bitmap bmp = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888);
            au.com.darkside.xserver.Cursor cursor = new au.com.darkside.xserver.Cursor(
                cid, xServer, client, bmp, 0, 0);
            xServer.addResource(cursor);
            if (client != null) client.addResource(cursor);
        }
    }
}
