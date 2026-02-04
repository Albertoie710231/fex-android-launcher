package com.termux.x11;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Window;

import androidx.annotation.Keep;
import androidx.preference.PreferenceManager;

/**
 * Stub MainActivity required by libXlorie.so JNI bindings.
 * All methods must match exactly what libXlorie expects.
 */
@Keep
public class MainActivity extends Activity {

    private static final String TAG = "Termux:X11-Stub";
    private static Handler handler = new Handler(Looper.getMainLooper());

    @Keep public static MainActivity instance;
    @Keep public LorieView lorieView;
    @Keep public static Window win;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        instance = this;
        win = getWindow();
        Log.d(TAG, "Stub MainActivity created");
    }

    @Keep
    public static MainActivity getInstance() {
        return instance;
    }

    // Called by native code - no parameters version
    @Keep
    public void clientConnectedStateChanged() {
        Log.d(TAG, "clientConnectedStateChanged()");
    }

    // Called by native code - with boolean parameter
    @Keep
    public void clientConnectedStateChanged(boolean connected) {
        Log.d(TAG, "clientConnectedStateChanged: " + connected);
    }

    @Keep
    public static void setClipboardText(String text) {
        Log.d(TAG, "setClipboardText");
    }

    @Keep
    public static String getClipboardText() {
        return "";
    }

    @Keep
    public void onPreferencesChanged() {
        Log.d(TAG, "onPreferencesChanged");
    }

    @Keep
    public static void onPreferencesChanged(String key) {
        Log.d(TAG, "onPreferencesChanged: " + key);
    }

    @Keep
    public static Context getContext() {
        return instance;
    }

    @Keep
    public static SharedPreferences getPrefs() {
        if (instance != null) {
            return PreferenceManager.getDefaultSharedPreferences(instance);
        }
        return null;
    }

    @Keep
    public static Window getWindow1() {
        return win;
    }

    @Keep
    public static LorieView getLorieView() {
        return instance != null ? instance.lorieView : null;
    }

    @Keep
    public static void setLorieView(LorieView view) {
        if (instance != null) {
            instance.lorieView = view;
        }
    }

    @Keep
    public static int getDpi() {
        return 96;
    }

    @Keep
    public static float getScale() {
        return 1.0f;
    }

    @Keep
    public static boolean getHardwareKbdScancodesWorkaround() {
        return false;
    }

    @Keep
    public static boolean getPointerCaptureEnabled() {
        return false;
    }

    @Keep
    public static void requestConnection() {
        Log.d(TAG, "requestConnection called");
        // Forward to LorieView's native requestConnection
        try {
            boolean result = LorieView.requestConnection();
            Log.d(TAG, "LorieView.requestConnection() returned: " + result);
        } catch (Exception e) {
            Log.e(TAG, "requestConnection failed", e);
        }
    }

    @Keep
    public static void post(Runnable r) {
        handler.post(r);
    }

    @Keep
    public static void postDelayed(Runnable r, long delay) {
        handler.postDelayed(r, delay);
    }

    @Keep
    public void runOnUiThread1(Runnable r) {
        handler.post(r);
    }

    @Keep
    public static boolean isConnected() {
        return LorieView.connected();
    }

    @Keep
    public static void connect(int fd) {
        LorieView.connect(fd);
    }
}
