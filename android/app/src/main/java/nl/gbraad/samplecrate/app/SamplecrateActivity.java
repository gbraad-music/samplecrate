package nl.gbraad.samplecrate.app;

import org.libsdl.app.SDLActivity;
import android.os.Bundle;
import android.os.Build;
import android.content.Intent;
import android.content.Context;
import android.content.IntentFilter;
import android.content.BroadcastReceiver;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.app.PendingIntent;
import android.util.Log;
import java.util.HashMap;

/**
 * Main activity for Samplecrate Android app.
 * Extends SDL's SDLActivity to provide the main game loop and rendering.
 */
public class SamplecrateActivity extends SDLActivity {
    private static final String TAG = "SamplecrateActivity";
    private static final String ACTION_USB_PERMISSION = "nl.gbraad.samplecrate.app.USB_PERMISSION";

    private UsbManager mUsbManager;
    private PendingIntent mPermissionIntent;

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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Log.i(TAG, "Starting Samplecrate activity");
        super.onCreate(savedInstanceState);

        // Set up USB device handling for MIDI controllers
        mUsbManager = (UsbManager) getSystemService(Context.USB_SERVICE);

        int flags = Build.VERSION.SDK_INT >= Build.VERSION_CODES.M
            ? PendingIntent.FLAG_IMMUTABLE
            : 0;
        mPermissionIntent = PendingIntent.getBroadcast(
            this, 0, new Intent(ACTION_USB_PERMISSION), flags);

        IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(mUsbReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(mUsbReceiver, filter);
        }

        // Check for already connected USB devices
        checkConnectedUsbDevices();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        try {
            unregisterReceiver(mUsbReceiver);
        } catch (IllegalArgumentException e) {
            // Receiver was not registered
            Log.w(TAG, "USB receiver was not registered");
        }
    }

    /**
     * Check for USB MIDI devices that are already connected
     */
    private void checkConnectedUsbDevices() {
        HashMap<String, UsbDevice> deviceList = mUsbManager.getDeviceList();
        Log.d(TAG, "Checking for connected USB devices: " + deviceList.size() + " device(s) found");

        for (UsbDevice device : deviceList.values()) {
            if (isMidiDevice(device)) {
                Log.d(TAG, "Found USB MIDI device: " + device.getDeviceName());
                if (!mUsbManager.hasPermission(device)) {
                    Log.d(TAG, "Requesting permission for: " + device.getDeviceName());
                    mUsbManager.requestPermission(device, mPermissionIntent);
                } else {
                    Log.d(TAG, "Already have permission for: " + device.getDeviceName());
                }
            }
        }
    }

    /**
     * Check if a USB device is a MIDI device
     * USB Class 1 (Audio), Subclass 3 (MIDI Streaming)
     */
    private boolean isMidiDevice(UsbDevice device) {
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            if (device.getInterface(i).getInterfaceClass() == 1 &&
                device.getInterface(i).getInterfaceSubclass() == 3) {
                return true;
            }
        }
        return false;
    }

    /**
     * BroadcastReceiver for USB device events
     */
    private final BroadcastReceiver mUsbReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);

            if (device == null) return;

            if (ACTION_USB_PERMISSION.equals(action)) {
                synchronized (this) {
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                        Log.d(TAG, "USB permission granted for: " + device.getDeviceName());
                        // Permission granted - RtMidi will handle the device
                    } else {
                        Log.d(TAG, "USB permission denied for: " + device.getDeviceName());
                    }
                }
            } else if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action)) {
                Log.d(TAG, "USB device attached: " + device.getDeviceName());
                if (isMidiDevice(device)) {
                    if (!mUsbManager.hasPermission(device)) {
                        Log.d(TAG, "Requesting permission for MIDI device");
                        mUsbManager.requestPermission(device, mPermissionIntent);
                    }
                }
            } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
                Log.d(TAG, "USB device detached: " + device.getDeviceName());
                // RtMidi will handle device removal
            }
        }
    };

}
