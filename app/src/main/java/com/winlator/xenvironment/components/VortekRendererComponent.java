package com.winlator.xenvironment.components;

import android.util.Log;
import androidx.annotation.Keep;
import com.winlator.xconnector.ConnectedClient;
import com.winlator.xconnector.ConnectionHandler;
import com.winlator.xconnector.RequestHandler;
import com.winlator.xconnector.UnixSocketConfig;
import com.winlator.xconnector.XConnectorEpoll;
import com.winlator.xconnector.XInputStream;
import java.io.IOException;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Vortek Vulkan renderer component.
 *
 * This is a port of Winlator's VortekRendererComponent to work with
 * our Steam Launcher. It handles:
 * - Unix socket server for Vortek client connections
 * - JNI calls to libvortekrenderer.so for Vulkan passthrough
 *
 * The native library expects this exact class path for JNI bindings.
 */
public class VortekRendererComponent implements ConnectionHandler, RequestHandler {
    private static final String TAG = "VortekRenderer";

    // Vulkan version: 1.3.128
    public static final int VK_MAX_VERSION = vkMakeVersion(1, 3, 128);

    private XConnectorEpoll connector;
    private final Options options;
    private final UnixSocketConfig socketConfig;
    private final ConcurrentHashMap<Integer, Long> clientContexts = new ConcurrentHashMap<>();

    // Window info callback - set by the app
    private WindowInfoProvider windowInfoProvider;

    // Track Vulkan initialization state
    private boolean vulkanInitialized = false;

    // JNI native methods - must match libvortekrenderer.so exports
    private native long createVkContext(int fd, Options options);
    private native void destroyVkContext(long contextPtr);
    private native void initVulkanWrapper(String nativeLibDir, String libvulkanPath);

    static {
        try {
            System.loadLibrary("vortekrenderer");
            Log.i(TAG, "libvortekrenderer.so loaded successfully");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Failed to load libvortekrenderer.so: " + e.getMessage());
        }
    }

    /**
     * Options for Vortek renderer configuration.
     * This class is passed to native code, field names must match.
     */
    @Keep
    public static class Options {
        public int vkMaxVersion = VK_MAX_VERSION;
        public short maxDeviceMemory = 4096;  // MB
        public short imageCacheSize = 256;    // MB
        public byte resourceMemoryType = 0;
        public String[] exposedDeviceExtensions = null;
        public String libvulkanPath = null;
    }

    /**
     * Interface for providing window information to native code.
     */
    public interface WindowInfoProvider {
        int getWindowWidth(int windowId);
        int getWindowHeight(int windowId);
        long getWindowHardwareBuffer(int windowId);
        void updateWindowContent(int windowId);
    }

    public VortekRendererComponent(String socketPath, String nativeLibDir, Options options) {
        this.socketConfig = UnixSocketConfig.create(
            socketPath.substring(0, socketPath.lastIndexOf('/')),
            socketPath.substring(socketPath.lastIndexOf('/') + 1)
        );
        this.options = options != null ? options : new Options();

        // Initialize Vulkan wrapper with library paths
        Log.i(TAG, "Initializing Vulkan wrapper: nativeLibDir=" + nativeLibDir + ", libvulkanPath=" + this.options.libvulkanPath);
        try {
            initVulkanWrapper(nativeLibDir, this.options.libvulkanPath);
            vulkanInitialized = true;
            Log.i(TAG, "Vulkan wrapper initialized successfully");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Failed to initialize Vulkan wrapper (UnsatisfiedLinkError): " + e.getMessage());
            vulkanInitialized = false;
        } catch (Exception e) {
            Log.e(TAG, "Failed to initialize Vulkan wrapper: " + e.getMessage());
            vulkanInitialized = false;
        }
    }

    /**
     * Check if Vulkan was initialized successfully.
     */
    public boolean isVulkanInitialized() {
        return vulkanInitialized;
    }

    public void setWindowInfoProvider(WindowInfoProvider provider) {
        this.windowInfoProvider = provider;
    }

    public void start() {
        if (connector != null) {
            Log.w(TAG, "Connector already running");
            return;
        }

        try {
            connector = new XConnectorEpoll(socketConfig, this, this);
            connector.setInitialInputBufferCapacity(1);
            connector.setInitialOutputBufferCapacity(0);
            connector.start();
            Log.i(TAG, "Vortek server started on: " + socketConfig.path);
        } catch (Exception e) {
            Log.e(TAG, "Failed to start Vortek server: " + e.getMessage());
            connector = null;
        }
    }

    public void stop() {
        if (connector != null) {
            connector.destroy();
            connector = null;
        }

        // Destroy all client contexts
        for (Long contextPtr : clientContexts.values()) {
            try {
                destroyVkContext(contextPtr);
            } catch (Exception e) {
                Log.w(TAG, "Error destroying context: " + e.getMessage());
            }
        }
        clientContexts.clear();

        Log.i(TAG, "Vortek server stopped");
    }

    // === ConnectionHandler implementation ===

    @Override
    public void handleNewConnection(ConnectedClient client) {
        Log.d(TAG, "New Vortek client connected: fd=" + client.fd);
    }

    @Override
    public void handleConnectionShutdown(ConnectedClient client) {
        Log.d(TAG, "Vortek client disconnected: fd=" + client.fd);
        if (client.getTag() != null) {
            long contextPtr = (Long) client.getTag();
            try {
                destroyVkContext(contextPtr);
            } catch (Exception e) {
                Log.w(TAG, "Error destroying context on disconnect: " + e.getMessage());
            }
            clientContexts.remove(client.fd);
        }
    }

    // === RequestHandler implementation ===

    @Override
    public boolean handleRequest(ConnectedClient client) throws IOException {
        XInputStream inputStream = client.getInputStream();
        if (inputStream.available() < 1) {
            return false;
        }

        byte requestCode = inputStream.readByte();
        if (requestCode == 1) {
            // Guard: Check if Vulkan was initialized successfully
            if (!vulkanInitialized) {
                Log.e(TAG, "Cannot create context: Vulkan not initialized");
                connector.killConnection(client);
                return true;
            }

            // Guard: Check if WindowInfoProvider is set (required for rendering)
            if (windowInfoProvider == null) {
                Log.e(TAG, "Cannot create context: WindowInfoProvider not set");
                connector.killConnection(client);
                return true;
            }

            // Create Vulkan context for this client
            Log.d(TAG, "Creating Vulkan context for fd=" + client.fd + " (WindowInfoProvider: set)");
            Log.d(TAG, "Options: vkMaxVersion=" + options.vkMaxVersion +
                       ", maxDeviceMemory=" + options.maxDeviceMemory +
                       ", imageCacheSize=" + options.imageCacheSize +
                       ", libvulkanPath=" + options.libvulkanPath);
            try {
                long contextPtr = createVkContext(client.fd, options);
                Log.d(TAG, "createVkContext returned: " + contextPtr + " (0x" + Long.toHexString(contextPtr) + ")");
                // Note: contextPtr is a native pointer, which can have the high bit set
                // on 64-bit systems. Check for != 0 instead of > 0.
                if (contextPtr != 0) {
                    client.setTag(Long.valueOf(contextPtr));
                    clientContexts.put(client.fd, contextPtr);
                    Log.i(TAG, "Created Vulkan context: 0x" + Long.toHexString(contextPtr));
                } else {
                    Log.e(TAG, "Failed to create Vulkan context (returned null)");
                    connector.killConnection(client);
                }
            } catch (Exception e) {
                Log.e(TAG, "Exception creating Vulkan context: " + e.getMessage());
                connector.killConnection(client);
            }
        }

        return true;
    }

    // === Native callbacks (called from libvortekrenderer.so) ===

    @Keep
    private int getWindowWidth(int windowId) {
        if (windowInfoProvider != null) {
            return windowInfoProvider.getWindowWidth(windowId);
        }
        return 1920; // Default fallback
    }

    @Keep
    private int getWindowHeight(int windowId) {
        if (windowInfoProvider != null) {
            return windowInfoProvider.getWindowHeight(windowId);
        }
        return 1080; // Default fallback
    }

    @Keep
    private long getWindowHardwareBuffer(int windowId) {
        if (windowInfoProvider != null) {
            return windowInfoProvider.getWindowHardwareBuffer(windowId);
        }
        return 0;
    }

    @Keep
    private void updateWindowContent(int windowId) {
        if (windowInfoProvider != null) {
            windowInfoProvider.updateWindowContent(windowId);
        }
    }

    // === Helper methods ===

    public static int vkMakeVersion(int major, int minor, int patch) {
        return (major << 22) | (minor << 12) | patch;
    }

    public String getSocketPath() {
        return socketConfig != null ? socketConfig.path : null;
    }
}
