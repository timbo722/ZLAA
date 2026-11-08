package io.github.zlaa;

import android.content.Intent;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * ZLAAActivity - SDL's activity, plus what the start screen needs from Android to find the player's game files
 * where they are: a folder picker and "All files access" (the game's 6 GB are read in place, never copied).
 * The native side (host/main.c) calls these static methods through JNI and polls takePickedFolder().
 */
public class ZLAAActivity extends SDLActivity {
    private static final String TAG = "ZLAA";
    private static final int PICK_FOLDER = 0x4C41;

    /** The folder chosen: a file path; "" when the picker was cancelled or the folder can't be read as a path;
     *  null while there is no answer (yet). */
    private static volatile String picked;

    /** May the app read files anywhere in shared storage? */
    public static boolean hasFilesAccess() {
        return Environment.isExternalStorageManager();
    }

    /** Opens Android's settings page where the player allows "All files access" for this app. */
    public static void requestFilesAccess() {
        mSingleton.runOnUiThread(() -> {
            try {
                Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                      Uri.parse("package:" + mSingleton.getPackageName()));
                mSingleton.startActivity(i);
            } catch (Exception e) {
                mSingleton.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        });
    }

    /** Opens the folder picker; the answer comes to takePickedFolder(). */
    public static void pickFolder() {
        picked = null;
        mSingleton.runOnUiThread(() -> {
            try {
                mSingleton.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER);
            } catch (Exception e) {
                Log.e(TAG, "no folder picker", e);
                picked = "";
            }
        });
    }

    /** The picker's answer once (see picked), then null again. */
    public static String takePickedFolder() {
        String p = picked;
        picked = null;
        return p;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != PICK_FOLDER) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        String path = "";
        if (resultCode == RESULT_OK && data != null && data.getData() != null) path = treeToPath(data.getData());
        picked = path;
    }

    /** A folder chosen in the picker ("content://...tree/primary%3ALADump%2F1.0.0") as a file path
     *  (/storage/emulated/0/LADump/1.0.0); a memory card's volume id (1234-ABCD) is /storage/1234-ABCD. */
    private static String treeToPath(Uri tree) {
        try {
            String id = DocumentsContract.getTreeDocumentId(tree);
            int colon = id.indexOf(':');
            if (colon < 0) return "";
            String volume = id.substring(0, colon), rel = id.substring(colon + 1);
            String root = volume.equals("primary") ? Environment.getExternalStorageDirectory().getPath()
                                                   : "/storage/" + volume;
            return rel.isEmpty() ? root : root + "/" + rel;
        } catch (Exception e) {
            Log.e(TAG, "folder " + tree + " has no path", e);
            return "";
        }
    }
}
