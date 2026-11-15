package dev.sekiro.nativeport;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Bundle;
import android.system.Os;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

public final class PreviewActivity extends NativeActivity {
    private static final int PICK_TEXTURE = 41;
    private static final int MAX_TEXTURE_BYTES = 64 * 1024 * 1024;
    private static final String TAG = "SekiroNative";

    static {
        System.loadLibrary("sekiro_native");
    }

    private AlertDialog issue;
    private String lastIssue;
    private boolean selecting;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setFormat(PixelFormat.RGBA_8888);
        selecting = state != null && state.getBoolean("selectingTexture");
        applyImmersive(getWindow());
    }

    @Override
    protected void onSaveInstanceState(Bundle state) {
        state.putBoolean("selectingTexture", selecting);
        super.onSaveInstanceState(state);
    }

    @Override
    public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused) {
            applyImmersive(getWindow());
        }
    }

    @SuppressWarnings("deprecation")
    private static void applyImmersive(Window window) {
        if (window == null) {
            return;
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                controller.hide(WindowInsets.Type.systemBars());
                return;
            }
        }
        window.getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    // The NativeActivity worker calls this through JNI. Dialogs use a separate Android window.
    @SuppressWarnings("unused")
    private void onPreviewIssue(String message) {
        runOnUiThread(() -> showIssue(message));
    }

    private void showIssue(String message) {
        lastIssue = message;
        if (isFinishing() || isDestroyed() || selecting) {
            return;
        }
        if (issue != null) {
            issue.dismiss();
        }
        issue = new AlertDialog.Builder(this)
                .setTitle("Sekiro texture preview")
                .setMessage(message)
                .setPositiveButton("Open TPF file", (dialog, which) -> chooseTexture())
                .setNegativeButton("Close app", (dialog, which) -> finish())
                .setCancelable(false)
                .create();
        issue.show();
        applyImmersive(issue.getWindow());
    }

    @SuppressWarnings("deprecation")
    private void chooseTexture() {
        selecting = true;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("*/*");
        try {
            startActivityForResult(intent, PICK_TEXTURE);
        } catch (ActivityNotFoundException error) {
            selecting = false;
            Log.e(TAG, "Android document picker unavailable", error);
            showIssue("The Android file picker is unavailable on this device.");
        }
    }

    @Override
    @SuppressWarnings("deprecation")
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_TEXTURE) {
            return;
        }
        if (result != RESULT_OK || data == null || data.getData() == null) {
            selecting = false;
            showIssue(lastIssue == null ? "Open inspection.tpf to load the texture preview." : lastIssue);
            return;
        }

        new Thread(() -> {
            try {
                importTexture(data);
                runOnUiThread(() -> {
                    selecting = false;
                    if (!isFinishing() && !isDestroyed()) {
                        recreate();
                    }
                });
            } catch (Exception error) {
                Log.e(TAG, "TPF import failed", error);
                runOnUiThread(() -> {
                    selecting = false;
                    showIssue("Could not import the texture. " + error.getMessage());
                });
            }
        }, "SekiroTpfImport").start();
    }

    private void importTexture(Intent selection) throws Exception {
        File staged = File.createTempFile("preview-", ".tpf", getFilesDir());
        try {
            int total = 0;
            try (InputStream input = getContentResolver().openInputStream(selection.getData());
                 FileOutputStream output = new FileOutputStream(staged)) {
                if (input == null) {
                    throw new IOException("Android could not open the selected file.");
                }
                byte[] buffer = new byte[32 * 1024];
                int size;
                while ((size = input.read(buffer)) != -1) {
                    total += size;
                    if (total > MAX_TEXTURE_BYTES) {
                        throw new IOException("TPF files must be at most 64 MiB.");
                    }
                    output.write(buffer, 0, size);
                }
                output.getFD().sync();
            }
            String decodeError = validateTexture(staged.getPath());
            if (decodeError != null) {
                throw new IOException(decodeError);
            }
            Os.rename(staged.getPath(), new File(getFilesDir(), "inspection.tpf").getPath());
            Log.i(TAG, "Imported validated TPF into private app storage (" + total + " bytes)");
        } finally {
            if (staged.exists() && !staged.delete()) {
                Log.w(TAG, "Could not remove temporary TPF import");
            }
        }
    }

    private static native String validateTexture(String path);

    @Override
    protected void onDestroy() {
        if (issue != null) {
            issue.dismiss();
        }
        super.onDestroy();
    }
}
