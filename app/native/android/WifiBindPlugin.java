package ir.smartcarlock.carcontrol;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Handler;
import android.os.Looper;

import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;

import java.util.concurrent.atomic.AtomicBoolean;

/**
 * وای‌فای دستگاه (CarControl) اینترنت ندارد؛ اندروید در این حالت ترافیک برنامه را به سیم‌کارت می‌فرستد
 * و درخواست به 192.168.4.1 نمی‌رسد. این پلاگین پروسه‌ی برنامه را به شبکه‌ی وای‌فای می‌چسباند.
 */
@CapacitorPlugin(name = "WifiBind")
public class WifiBindPlugin extends Plugin {
    private ConnectivityManager.NetworkCallback callback;

    private ConnectivityManager cm() {
        return (ConnectivityManager) getContext().getSystemService(Context.CONNECTIVITY_SERVICE);
    }

    @PluginMethod
    public void bind(final PluginCall call) {
        final ConnectivityManager cm = cm();
        if (callback != null) { call.resolve(); return; }

        final AtomicBoolean done = new AtomicBoolean(false);
        NetworkRequest req = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .build();

        callback = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network network) {
                cm.bindProcessToNetwork(network);
                if (done.compareAndSet(false, true)) call.resolve();
            }
            @Override public void onLost(Network network) {
                cm.bindProcessToNetwork(null);
            }
        };

        try {
            cm.requestNetwork(req, callback);
        } catch (Exception e) {
            callback = null;
            if (done.compareAndSet(false, true)) call.resolve();
            return;
        }
        // اگر وای‌فایی وصل نیست، بیشتر از ۲.۵ ثانیه منتظر نمی‌مانیم
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            if (done.compareAndSet(false, true)) call.resolve();
        }, 2500);
    }

    @PluginMethod
    public void unbind(PluginCall call) {
        try {
            ConnectivityManager cm = cm();
            cm.bindProcessToNetwork(null);
            if (callback != null) { cm.unregisterNetworkCallback(callback); callback = null; }
        } catch (Exception ignored) { }
        call.resolve();
    }
}
