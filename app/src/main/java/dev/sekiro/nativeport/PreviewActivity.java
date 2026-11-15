package dev.sekiro.nativeport;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.res.Configuration;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Bundle;
import android.system.Os;
import android.util.Log;
import android.view.DisplayCutout;
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
    private static final int PICK_MODEL = 42;
    private static final int MAX_ASSET_BYTES = 64 * 1024 * 1024;
    private static final String TAG = "SekiroNative";
    private static final String MENU_TITLE = "Sekiro asset preview · " + BuildConfig.VERSION_NAME;

    static {
        System.loadLibrary("sekiro_native");
    }

    private AlertDialog issue;
    private WindowInsetsController insetsController;
    private final WindowInsetsController.OnControllableInsetsChangedListener onInsetsControl =
            (controller, types) -> {
                int bars = types & (WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                Log.i(TAG, "Controllable system bar types: 0x" + Integer.toHexString(bars));
                if (bars != 0) {
                    reapplyFullscreen();
                }
            };
    private final Runnable applyFullscreen = this::requestFullscreen;
    private final Runnable checkFullscreen = this::verifyFullscreen;
    private String previousBars;
    private String lastIssue;
    private boolean selecting;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setFormat(PixelFormat.RGBA_8888);
        selecting = state != null && state.getBoolean("selectingTexture");
        Log.i(TAG, MENU_TITLE + " (Android API " + Build.VERSION.SDK_INT + ")");
        configureFullscreen();
        reapplyFullscreen();
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
            reapplyFullscreen();
        } else {
            cancelFullscreenCallbacks();
        }
    }

    @Override
    public void onAttachedToWindow() {
        super.onAttachedToWindow();
        reapplyFullscreen();
    }

    @Override
    protected void onResume() {
        super.onResume();
        reapplyFullscreen();
    }

    @Override
    public void onConfigurationChanged(Configuration configuration) {
        super.onConfigurationChanged(configuration);
        getWindow().getDecorView().requestApplyInsets();
        reapplyFullscreen();
    }

    private void configureFullscreen() {
        Window window = getWindow();
        if (Build.VERSION.SDK_INT < 30) {
            window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        }
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams params = window.getAttributes();
            params.layoutInDisplayCutoutMode = Build.VERSION.SDK_INT >= 30
                    ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                    : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            window.setAttributes(params);
        }
        if (Build.VERSION.SDK_INT >= 29) {
            window.setStatusBarContrastEnforced(false);
            window.setNavigationBarContrastEnforced(false);
        }
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false);
        }
        window.getDecorView().setOnApplyWindowInsetsListener((view, insets) -> {
            DisplayCutout cutout = Build.VERSION.SDK_INT >= 28 ? insets.getDisplayCutout() : null;
            nativeSetCutout(cutout == null ? 0 : cutout.getSafeInsetLeft(),
                    cutout == null ? 0 : cutout.getSafeInsetTop(),
                    cutout == null ? 0 : cutout.getSafeInsetRight(),
                    cutout == null ? 0 : cutout.getSafeInsetBottom());
            if (Build.VERSION.SDK_INT >= 30) {
                String bars = "status visible=" + insets.isVisible(WindowInsets.Type.statusBars()) +
                        ", navigation visible=" + insets.isVisible(WindowInsets.Type.navigationBars());
                if (!bars.equals(previousBars)) {
                    Log.i(TAG, "Window insets: " + bars);
                    previousBars = bars;
                }
            }
            return view.onApplyWindowInsets(insets);
        });
        window.getDecorView().requestApplyInsets();
    }

    private void cancelFullscreenCallbacks() {
        View decor = getWindow().getDecorView();
        decor.removeCallbacks(applyFullscreen);
        decor.removeCallbacks(checkFullscreen);
    }

    private void reapplyFullscreen() {
        cancelFullscreenCallbacks();
        getWindow().getDecorView().post(applyFullscreen);
    }

    private void requestFullscreen() {
        Window window = getWindow();
        View decor = window.getDecorView();
        if (isDestroyed() || !decor.isAttachedToWindow() || !hasWindowFocus()) {
            return;
        }
        if (Build.VERSION.SDK_INT >= 30 && window.getInsetsController() != null) {
            WindowInsetsController controller = window.getInsetsController();
            if (insetsController != controller) {
                if (insetsController != null) {
                    insetsController.removeOnControllableInsetsChangedListener(onInsetsControl);
                }
                insetsController = controller;
                controller.addOnControllableInsetsChangedListener(onInsetsControl);
            }
            hideSystemBars(controller);
            decor.removeCallbacks(checkFullscreen);
            decor.postDelayed(checkFullscreen, 750);
        } else {
            applyLegacyImmersive(decor);
        }
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars(WindowInsetsController controller) {
        if (Build.VERSION.SDK_INT == 30) {
            // Android 11 can lose the controller's behaviour on a window attribute update.
            View decor = getWindow().getDecorView();
            decor.setSystemUiVisibility((decor.getSystemUiVisibility() & ~View.SYSTEM_UI_FLAG_IMMERSIVE)
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        } else {
            controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
        controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
    }

    @SuppressWarnings("deprecation")
    private static void applyLegacyImmersive(View decor) {
        int flags = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_FULLSCREEN;
        if (Build.VERSION.SDK_INT < 30) {
            flags |= View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
        }
        decor.setSystemUiVisibility((decor.getSystemUiVisibility() & ~View.SYSTEM_UI_FLAG_IMMERSIVE) | flags);
    }

    private void verifyFullscreen() {
        if (Build.VERSION.SDK_INT < 30 || isDestroyed() || !hasWindowFocus()) {
            return;
        }
        View decor = getWindow().getDecorView();
        WindowInsets insets = decor.getRootWindowInsets();
        if (insets != null && (insets.isVisible(WindowInsets.Type.statusBars()) ||
                               insets.isVisible(WindowInsets.Type.navigationBars()))) {
            Log.w(TAG, "System bars still reported visible after fullscreen request; applying compatibility flags");
            applyLegacyImmersive(decor);
            if (insetsController != null) {
                hideSystemBars(insetsController);
            }
        }
    }

    private static native void nativeSetCutout(int left, int top, int right, int bottom);


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
                .setTitle(MENU_TITLE)
                .setMessage(message)
                .setPositiveButton("Open TPF", (dialog, which) -> chooseAsset(PICK_TEXTURE))
                .setNeutralButton("Open FLVER", (dialog, which) -> chooseAsset(PICK_MODEL))
                .setNegativeButton("Close app", (dialog, which) -> finish())
                .setCancelable(false)
                .create();
        issue.show();

    }

    @SuppressWarnings("deprecation")
    private void chooseAsset(int request) {
        selecting = true;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("*/*");
        try {
            startActivityForResult(intent, request);
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
        if (request != PICK_TEXTURE && request != PICK_MODEL) {
            return;
        }
        if (result != RESULT_OK || data == null || data.getData() == null) {
            selecting = false;
            if (lastIssue != null) {
                showIssue(lastIssue);
            } else if (!new File(getFilesDir(), "inspection.tpf").exists() &&
                       !new File(getFilesDir(), "inspection.flver").exists()) {
                showIssue("Open a TPF texture or FLVER model from your local Sekiro files.");
            }
            return;
        }

        new Thread(() -> {
            try {
                importAsset(data, request == PICK_MODEL);
                runOnUiThread(() -> {
                    selecting = false;
                    if (!isFinishing() && !isDestroyed()) {
                        recreate();
                    }
                });
            } catch (Exception error) {
                Log.e(TAG, "Asset import failed", error);
                runOnUiThread(() -> {
                    selecting = false;
                    showIssue("Could not import the asset. " + error.getMessage());
                });
            }
        }, "SekiroAssetImport").start();
    }

    private void importAsset(Intent selection, boolean model) throws Exception {
        File staged = File.createTempFile("preview-", ".asset", getFilesDir());
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
                    if (total > MAX_ASSET_BYTES) {
                        throw new IOException("Preview assets must be at most 64 MiB.");
                    }
                    output.write(buffer, 0, size);
                }
                output.getFD().sync();
            }
            String decodeError = validateAsset(staged.getPath(), model);
            if (decodeError != null) {
                throw new IOException(decodeError);
            }
            String name = model ? "inspection.flver" : "inspection.tpf";
            Os.rename(staged.getPath(), new File(getFilesDir(), name).getPath());
            if (model) {
                new File(getFilesDir(), "texture-only").delete();
            }
            Log.i(TAG, "Imported " + name + " into private app storage (" + total + " bytes)");
        } finally {
            if (staged.exists() && !staged.delete()) {
                Log.w(TAG, "Could not remove temporary asset import");
            }
        }
    }

    private static native String validateAsset(String path, boolean model);

    private boolean hasPreviewAsset(String localName, String bundledName) {
        if (new File(getFilesDir(), localName).isFile()) {
            return true;
        }
        try (InputStream asset = getAssets().open("sekiro/" + bundledName)) {
            return true;
        } catch (IOException missing) {
            return false;
        }
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        if (selecting) {
            return;
        }
        File mode = new File(getFilesDir(), "texture-only");
        boolean textureOnly = mode.exists();
        boolean canSwitch = hasPreviewAsset("inspection.flver", "model.flver") &&
                            hasPreviewAsset("inspection.tpf", "preview.tpf");
        AlertDialog.Builder menu = new AlertDialog.Builder(this)
                .setTitle(MENU_TITLE)
                .setPositiveButton("Open TPF", (dialog, which) -> chooseAsset(PICK_TEXTURE))
                .setNeutralButton("Open FLVER", (dialog, which) -> chooseAsset(PICK_MODEL))
                .setNegativeButton("Close app", (dialog, which) -> finish());
        String[] actions = canSwitch ? new String[]{"Retry fullscreen", textureOnly ? "Show model" : "Show texture"}
                                     : new String[]{"Retry fullscreen"};
        menu.setItems(actions, (dialog, which) -> {
            if (which == 0) {
                reapplyFullscreen();
            } else {
                try {
                    if (!(textureOnly ? mode.delete() : mode.createNewFile())) {
                        throw new IOException("Could not change preview mode.");
                    }
                    recreate();
                } catch (IOException error) {
                    showIssue(error.getMessage());
                }
            }
        });
        issue = menu.show();
    }

    @Override
    protected void onDestroy() {
        cancelFullscreenCallbacks();
        if (Build.VERSION.SDK_INT >= 30 && insetsController != null) {
            insetsController.removeOnControllableInsetsChangedListener(onInsetsControl);
        }
        if (issue != null) {
            issue.dismiss();
        }
        super.onDestroy();
    }
}
