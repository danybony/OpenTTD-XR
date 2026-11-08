/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

package org.openttd.xr;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.ClipData;
import android.content.ContentValues;
import android.content.Intent;
import android.content.pm.ConfigurationInfo;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.hardware.input.InputManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.provider.MediaStore;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.InputDevice;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Unpacks the game data on the first launch of each version, then lets SDL start the game from libmain.so.
 * It also starts the test log with what it knows of the device, notes the life of the activity in it, and shares it.
 */
public class OpenTTDActivity extends SDLActivity {
	private static final String TAG = "OpenTTD";
	static final String TEST_LOG = "xr-test.log";
	static final String PREVIOUS_TEST_LOG = "xr-test.prev.log";

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		startTestLog();
		try {
			unpackGameData();
		} catch (IOException | PackageManager.NameNotFoundException e) {
			Log.e(TAG, "Unpacking the game data failed", e);
			testLog("unpacking the game data failed: " + e);
		}
		super.onCreate(savedInstanceState);
		describeDisplay("display");
	}

	@Override
	protected void onStart() {
		super.onStart();
		testLog("activity: started");
	}

	@Override
	protected void onResume() {
		super.onResume();
		testLog("activity: resumed");
	}

	@Override
	protected void onPause() {
		testLog("activity: paused");
		super.onPause();
	}

	@Override
	protected void onStop() {
		testLog("activity: stopped");
		super.onStop();
	}

	@Override
	protected void onDestroy() {
		testLog("activity: destroyed");
		super.onDestroy();
	}

	@Override
	public void onWindowFocusChanged(boolean hasFocus) {
		testLog("activity: focus " + (hasFocus ? "gained" : "lost"));
		super.onWindowFocusChanged(hasFocus);
	}

	@Override
	public void onConfigurationChanged(Configuration newConfig) {
		testLog("activity: configuration changed: " + describeConfiguration(newConfig));
		super.onConfigurationChanged(newConfig);
		describeDisplay("display after the change");
	}

	@Override
	public void onMultiWindowModeChanged(boolean isInMultiWindowMode, Configuration newConfig) {
		testLog("activity: multi window mode " + isInMultiWindowMode);
		super.onMultiWindowModeChanged(isInMultiWindowMode, newConfig);
	}

	@Override
	public void onTrimMemory(int level) {
		testLog("activity: trim memory level " + level);
		super.onTrimMemory(level);
	}

	@Override
	public void onLowMemory() {
		testLog("activity: low memory");
		super.onLowMemory();
	}

	/** Add a line to the test log; the game appends to the same file. */
	static synchronized void testLog(String line) {
		if (mSingleton == null) return;
		final File file = new File(mSingleton.getFilesDir(), TEST_LOG);
		final String time = new SimpleDateFormat("HH:mm:ss.SSS", Locale.ROOT).format(new Date());
		try (OutputStream out = new FileOutputStream(file, true)) {
			out.write((time + " " + line + "\n").getBytes());
		} catch (IOException e) {
			Log.w(TAG, "Writing the test log failed", e);
		}
		Log.i(TAG + "-XRTest", line);
	}

	/** Keep the last session's log, and start a new one with all that the activity knows of the device. */
	private void startTestLog() {
		final File log = new File(getFilesDir(), TEST_LOG);
		if (log.exists()) log.renameTo(new File(getFilesDir(), PREVIOUS_TEST_LOG));
		mSingleton = this;
		try {
			final PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
			testLog("OpenTTD XR test log, app " + info.versionName + " (" + info.versionCode + ")");
		} catch (PackageManager.NameNotFoundException e) {
			testLog("OpenTTD XR test log");
		}
		testLog("device: " + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + ", " + Build.PRODUCT + ", hardware " + Build.HARDWARE
			+ (Build.VERSION.SDK_INT >= 31 ? ", SoC " + Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL : "") + ")");
		testLog("android: " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + "), build " + Build.DISPLAY + ", ABIs " + Arrays.toString(Build.SUPPORTED_ABIS));
		final ActivityManager activities = getSystemService(ActivityManager.class);
		final ConfigurationInfo gl = activities.getDeviceConfigurationInfo();
		final ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
		activities.getMemoryInfo(memory);
		testLog("opengl es " + gl.getGlEsVersion() + ", memory " + memory.totalMem / (1024 * 1024) + " MB total, " + memory.availMem / (1024 * 1024)
			+ " MB free, app limit " + activities.getMemoryClass() + "/" + activities.getLargeMemoryClass() + " MB");
		testLog("configuration: " + describeConfiguration(getResources().getConfiguration()));

		final List<String> features = new ArrayList<>();
		for (FeatureInfo feature : getPackageManager().getSystemAvailableFeatures()) {
			if (feature.name == null) continue;
			final String name = feature.name.toLowerCase(Locale.ROOT);
			if (name.contains("xr") || name.contains("vr") || name.contains("openxr") || name.contains("spatial") || name.contains("headtracking")) {
				features.add(feature.name + (feature.version != 0 ? "=" + feature.version : ""));
			}
		}
		testLog("xr and vr features: " + (features.isEmpty() ? "none" : String.join(" ", features)));

		describeInputDevices();
		final InputManager input = getSystemService(InputManager.class);
		input.registerInputDeviceListener(new InputManager.InputDeviceListener() {
			@Override public void onInputDeviceAdded(int id) { testLog("input device added: " + describeInputDevice(InputDevice.getDevice(id))); }
			@Override public void onInputDeviceRemoved(int id) { testLog("input device removed: " + id); }
			@Override public void onInputDeviceChanged(int id) { testLog("input device changed: " + describeInputDevice(InputDevice.getDevice(id))); }
		}, null);

		if (Build.VERSION.SDK_INT >= 29) {
			final PowerManager power = getSystemService(PowerManager.class);
			testLog("thermal status: " + power.getCurrentThermalStatus());
			power.addThermalStatusListener(status -> testLog("thermal status: " + status));
		}

		if (Build.VERSION.SDK_INT >= 30) {
			for (ApplicationExitInfo exit : activities.getHistoricalProcessExitReasons(getPackageName(), 0, 3)) {
				testLog("earlier exit: " + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date(exit.getTimestamp()))
					+ " reason " + exit.getReason() + " status " + exit.getStatus() + " importance " + exit.getImportance()
					+ " pss " + exit.getPss() / 1024 + " MB, " + exit.getDescription());
			}
		}
	}

	private static String describeConfiguration(Configuration config) {
		final int type = config.uiMode & Configuration.UI_MODE_TYPE_MASK;
		return config.screenWidthDp + "x" + config.screenHeightDp + " dp, density " + config.densityDpi + " dpi, orientation " + config.orientation
			+ ", ui mode type " + type + (type == Configuration.UI_MODE_TYPE_VR_HEADSET ? " (vr headset)" : "")
			+ ", touchscreen " + config.touchscreen + ", navigation " + config.navigation + ", keyboard " + config.keyboard;
	}

	private void describeDisplay(String what) {
		final Display display = getWindowManager().getDefaultDisplay();
		final DisplayMetrics metrics = new DisplayMetrics();
		display.getRealMetrics(metrics);
		final StringBuilder modes = new StringBuilder();
		for (Display.Mode mode : display.getSupportedModes()) {
			modes.append(' ').append(mode.getPhysicalWidth()).append('x').append(mode.getPhysicalHeight()).append('@').append(mode.getRefreshRate());
		}
		testLog(what + ": " + display.getName() + " " + metrics.widthPixels + "x" + metrics.heightPixels + " at " + metrics.densityDpi + " dpi, "
			+ display.getRefreshRate() + " Hz, modes" + modes + ", window " + getWindow().getDecorView().getWidth() + "x" + getWindow().getDecorView().getHeight());
	}

	private static void describeInputDevices() {
		for (int id : InputDevice.getDeviceIds()) testLog("input device: " + describeInputDevice(InputDevice.getDevice(id)));
	}

	private static String describeInputDevice(InputDevice device) {
		if (device == null) return "gone";
		return device.getId() + " \"" + device.getName() + "\" sources 0x" + Integer.toHexString(device.getSources())
			+ " vendor 0x" + Integer.toHexString(device.getVendorId()) + " product 0x" + Integer.toHexString(device.getProductId())
			+ (device.isVirtual() ? " virtual" : "") + (device.isExternal() ? " external" : "");
	}

	/** Files that go with a test log: this session's, the previous one's and the latest crash report with its save. */
	private List<File> testFiles() {
		final List<File> files = new ArrayList<>();
		for (String name : new String[] {TEST_LOG, PREVIOUS_TEST_LOG}) {
			final File file = new File(getFilesDir(), name);
			if (file.exists()) files.add(file);
		}
		final File[] crashes = TestLogProvider.personalDir(this).listFiles((dir, name) -> name.matches(TestLogProvider.CRASH_LOG));
		if (crashes != null && crashes.length > 0) {
			Arrays.sort(crashes, (a, b) -> b.getName().compareTo(a.getName()));
			files.add(crashes[0]);
			final File save = new File(crashes[0].getParentFile(), crashes[0].getName().replace(".json.log", ".sav"));
			if (save.exists() && save.length() < 8 * 1024 * 1024) files.add(save);
		}
		return files;
	}

	/** Called by the game from the 3D menu: copy the test files to Downloads, and offer them to whatever app the tester picks. */
	public void shareTestLog() {
		runOnUiThread(() -> {
			final List<File> files = testFiles();
			testLog("sharing " + files.size() + " files");
			if (Build.VERSION.SDK_INT >= 29) copyToDownloads(files);

			final ArrayList<Uri> uris = new ArrayList<>();
			for (File file : files) uris.add(TestLogProvider.uriFor(file));
			if (uris.isEmpty()) return;
			final Intent send = new Intent(Intent.ACTION_SEND_MULTIPLE);
			send.setType("*/*");
			send.putExtra(Intent.EXTRA_SUBJECT, "OpenTTD XR test log, " + Build.MANUFACTURER + " " + Build.MODEL);
			send.putExtra(Intent.EXTRA_TEXT, "OpenTTD XR test log from " + Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE);
			send.putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris);
			final ClipData clip = ClipData.newRawUri(null, uris.get(0));
			for (int i = 1; i < uris.size(); i++) clip.addItem(new ClipData.Item(uris.get(i)));
			send.setClipData(clip);
			send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
			try {
				startActivity(Intent.createChooser(send, "Share the OpenTTD XR test log"));
			} catch (RuntimeException e) {
				testLog("share sheet failed: " + e);
			}
		});
	}

	/** Also put a copy where a file manager finds it, in case no app can take the files. */
	private void copyToDownloads(List<File> files) {
		final String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date());
		for (File file : files) {
			final ContentValues values = new ContentValues();
			values.put(MediaStore.MediaColumns.DISPLAY_NAME, "openttd-xr-" + stamp + "-" + file.getName());
			values.put(MediaStore.MediaColumns.MIME_TYPE, TestLogProvider.typeOf(file.getName()));
			values.put(MediaStore.MediaColumns.RELATIVE_PATH, "Download/OpenTTD-XR");
			try {
				final Uri target = getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
				if (target == null) continue;
				try (InputStream in = new java.io.FileInputStream(file); OutputStream out = getContentResolver().openOutputStream(target)) {
					copy(in, out);
				}
			} catch (IOException | RuntimeException e) {
				testLog("copying " + file.getName() + " to Downloads failed: " + e);
				return;
			}
		}
		testLog("copied to Download/OpenTTD-XR");
	}

	@Override
	protected String[] getLibraries() {
		return new String[] {"SDL2", "main"};
	}

	@Override
	protected String[] getArguments() {
		return new String[] {"-v", "sdl-opengl", "-b", "32bpp-optimized", "-m", "null", "-s", "null", "-d", "driver=1"};
	}

	private void unpackGameData() throws IOException, PackageManager.NameNotFoundException {
		final long version = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
		final File marker = new File(getFilesDir(), "game.version");
		if (marker.exists() && readAll(marker).equals(Long.toString(version))) return;

		deleteRecursively(new File(getFilesDir(), "game"));
		copyAssets(getAssets(), "game", getFilesDir());
		seedConfig();

		try (OutputStream out = new FileOutputStream(marker)) {
			out.write(Long.toString(version).getBytes());
		}
	}

	/** First-launch settings only; the game owns the file afterwards. */
	private void seedConfig() throws IOException {
		final File config = new File(getFilesDir(), "config/openttd/openttd.cfg");
		if (config.exists()) return;
		config.getParentFile().mkdirs();
		try (InputStream in = getAssets().open("openttd.cfg"); OutputStream out = new FileOutputStream(config)) {
			copy(in, out);
		}
	}

	private static void copyAssets(AssetManager assets, String path, File base) throws IOException {
		final String[] children = assets.list(path);
		if (children == null || children.length == 0) {
			final File target = new File(base, path);
			target.getParentFile().mkdirs();
			try (InputStream in = assets.open(path); OutputStream out = new FileOutputStream(target)) {
				copy(in, out);
			}
			return;
		}
		for (String child : children) copyAssets(assets, path + "/" + child, base);
	}

	private static void copy(InputStream in, OutputStream out) throws IOException {
		final byte[] buffer = new byte[64 * 1024];
		for (int read; (read = in.read(buffer)) > 0; ) out.write(buffer, 0, read);
	}

	private static String readAll(File file) throws IOException {
		try (InputStream in = new java.io.FileInputStream(file)) {
			return new String(in.readAllBytes());
		}
	}

	private static void deleteRecursively(File file) {
		final File[] children = file.listFiles();
		if (children != null) {
			for (File child : children) deleteRecursively(child);
		}
		file.delete();
	}
}
