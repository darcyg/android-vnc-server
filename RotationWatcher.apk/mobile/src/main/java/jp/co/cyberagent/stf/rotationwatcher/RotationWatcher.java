package jp.co.cyberagent.stf.rotationwatcher;

import jp.co.cyberagent.stf.compat.WindowManagerWrapper;

public class RotationWatcher extends Thread {
    private static final String TAG = "RotationWatcher";

    // This wrapper is copied from https://github.com/openstf/STFService.apk
    // to fix https://github.com/openstf/RotationWatcher.apk/issues/3
    private WindowManagerWrapper wmw = new WindowManagerWrapper();

    @Override
    public void run() {
        WindowManagerWrapper.RotationWatcher watcher = new WindowManagerWrapper.RotationWatcher() {
            @Override
            public void onRotationChanged(int rotation) {
                report(rotation);
            }
        };
        try {
            // Get the rotation we have right now.
            report(wmw.getRotation());

            // Watch for changes in rotation.
            wmw.watchRotation(watcher);

            // Just keep waiting.
            synchronized (this) {
                while (!isInterrupted()) {
                    wait();
                }
            }
        }
        catch (InterruptedException e) {
            // Okay
        }
        finally {
            // Sadly, wmw.removeRotationWatcher() is only available on API >= 18. Instead, we
            // must make sure that whole process dies, causing DeathRecipient to reap the
            // watcher.
//            wmw.removeRotationWatcher(androidRotationWatcher);
        }
    }

    private synchronized void report(int rotation) {
        // The internal values are very convenient, we can simply multiply by 90 to get the
        // actual degree.
        System.out.println(rotation * 90);
    }

    public static void main(String[] args) {
        try {
            RotationWatcher monitor = new RotationWatcher();
            monitor.start();
            monitor.join();
        }
        catch (InterruptedException e) {
            // Okay
        }
    }
}
