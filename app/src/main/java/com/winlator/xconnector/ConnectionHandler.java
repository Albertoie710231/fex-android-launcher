package com.winlator.xconnector;

/* loaded from: classes.dex */
public interface ConnectionHandler {
    void handleConnectionShutdown(ConnectedClient connectedClient);

    void handleNewConnection(ConnectedClient connectedClient);
}
