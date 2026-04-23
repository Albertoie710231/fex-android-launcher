package com.winlator.xenvironment;

/**
 * Base class for environment components (renderer, audio server, etc.).
 *
 * The full GameNative XEnvironment orchestrator isn't ported — we only
 * need the abstract surface for VortekRendererComponent. Subclasses
 * that reference .environment get a plain Object; unused in this port.
 */
public abstract class EnvironmentComponent {
    protected Object environment;
    public abstract void start();
    public abstract void stop();
}
