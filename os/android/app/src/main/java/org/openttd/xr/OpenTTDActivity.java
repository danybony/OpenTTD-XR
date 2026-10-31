/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

package org.openttd.xr;

import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/** Unpacks the game data on the first launch of each version, then lets SDL start the game from libmain.so. */
public class OpenTTDActivity extends SDLActivity {
	private static final String TAG = "OpenTTD";

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		try {
			unpackGameData();
		} catch (IOException | PackageManager.NameNotFoundException e) {
			Log.e(TAG, "Unpacking the game data failed", e);
		}
		super.onCreate(savedInstanceState);
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
