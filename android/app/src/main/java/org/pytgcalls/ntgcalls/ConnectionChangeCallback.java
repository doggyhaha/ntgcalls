package org.pytgcalls.ntgcalls;

public interface ConnectionChangeCallback {
    void onConnectionChange(long chatId, CallNetworkState state);
}
