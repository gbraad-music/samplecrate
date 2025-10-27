package com.samplecrate.app;

import org.libsdl.app.SDLActivity;
import android.os.Bundle;
import android.content.Intent;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.util.Log;

/**
 * Main activity for Samplecrate Android app.
 * Extends SDL's SDLActivity to provide the main game loop and rendering.
 */
public class SamplecrateActivity extends SDLActivity {
    private static final String TAG = "Samplecrate";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Log.i(TAG, "Starting Samplecrate activity");
        super.onCreate(savedInstanceState);

        // Handle USB MIDI device attachment
        Intent intent = getIntent();
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(intent.getAction())) {
            UsbDevice device = (UsbDevice) intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            if (device != null) {
                Log.i(TAG, "USB MIDI device attached: " + device.getDeviceName());
            }
        }
    }

    /**
     * Specify the libraries that SDL should load.
     * Order matters - SDL2 must be loaded before samplecrate.
     */
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL2",
            "samplecrate"
        };
    }

    /**
     * Get the main function name from the native library.
     * This is called by SDL to start the native code.
     */
    @Override
    protected String getMainFunction() {
        return "SDL_main";
    }

    /**
     * Arguments to pass to the main function.
     * You can customize this to pass command-line arguments to your C++ code.
     */
    @Override
    protected String[] getArguments() {
        return new String[0];
    }
}
